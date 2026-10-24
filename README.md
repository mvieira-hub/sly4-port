# sly4-port

Engenharia reversa de **Sly Cooper: Thieves in Time** (PS3, BCES01284), com um objetivo de longo prazo: um **port nativo para PC** (Windows), sem emulador.

O repositório guarda só o trabalho próprio: ferramentas, documentação dos formatos e mapas do executável. **Nenhum arquivo do jogo é distribuído.** Para usar, é preciso o seu próprio dump do disco em `PS3_GAME/`; o `.gitignore` impede que ele, ou qualquer coisa extraída dele, entre no repositório.

## Onde estamos

| Área | Estado | Documento |
|---|---|---|
| Contêineres (`.psarc`) | Extração completa | [tools/README.md](tools/README.md) §1 |
| Texturas (TEXR/GTF → DDS) | 49.981 texturas, todas DXT | §2 |
| Scripts (XMLB → XML/JSON) | Formato resolvido; 85% dos nomes recuperados | §3 |
| Executável (EBOOT, PPU) | Descriptografado via RPCS3; imports, syscalls, TOC e 144 tratadores de bloco mapeados no Ghidra | [GHIDRA.md](tools/docs/GHIDRA.md) |
| Malhas e fases (MESH/GINS → glTF) | 87 fases exportadas com hierarquia, materiais e texturas | §6 |
| Personagens (PlayStation EDGE) | Índices comprimidos e skinning decodificados a partir do código SPU | [EDGE.md](tools/docs/EDGE.md) |
| Animações (Havok 2011.3) | Todos os 4.320 clipes decodificam; exportados no glTF | [HAVOK.md](tools/docs/HAVOK.md) |
| Shaders (Cg do RSX) | 541 programas inventariados; estratégia de conversão definida | [SHADERS.md](tools/docs/SHADERS.md) |

## Roteiro do port nativo

O plano completo, com auditoria, arquitetura do runtime, fases e critérios de saída, está em **[docs/ROADMAP.md](docs/ROADMAP.md)**. Resumo:

O jogo tem três partes de código, e cada uma pede uma abordagem:

1. **PPU (lógica do jogo, ~15 MB de PowerPC 64):** recompilação estática para C/C++. Base existente: mapa de funções, TOC (r2 = `0xF50800`), imports por NID e tratadores de bloco nomeados.
2. **SPU (jobs paralelos: EDGE, Havok, áudio…):** reescrever cada job em C++ a partir da desmontagem. O descompressor EDGE já foi lido e reimplementado em Python (`tools/edge.py`).
3. **RSX (gráficos):** trocar os comandos do RSX e os shaders Cg por Vulkan/DirectX 12, partindo do inventário de shaders.

Em volta disso: reimplementar as bibliotecas do sistema (arquivos, áudio, controle, threads) que o executável importa.

Próximos passos imediatos:
- materiais completos (normal/especular) e animação dos objetos das fases, para validar o entendimento dos dados;
- lacunas do EDGE (`inputFmt` 0 e 8), LODs, contornos (`OUTL`), luzes e efeitos;
- começar a classificar as funções do EBOOT por subsistema (motor Sanzaru, Havok, EDGE, bibliotecas da Sony).

## Port nativo: compilar e rodar

O runtime C++20 (`runtime/`, `app/`) é compilado para Windows x86-64 dentro de um contêiner com MinGW-w64 (GCC 14) e SDL3. O resultado é um único `.exe`, sem DLLs.

```powershell
.\docker\build.ps1                  # Release -> build\mingw-release\sly4.exe
.\docker\build.ps1 -Config Debug    # Debug   -> build\mingw-debug\sly4.exe
.\build\mingw-release\sly4.exe      # janela até fechar (Esc fecha)
.\build\mingw-release\sly4.exe --frames 300   # teste: 300 quadros e sai (código 0 = ok)
```

Outras opções: `--no-vsync` e `--validation`. Esta última exige a camada de validação do Vulkan SDK; sem ela, o programa só avisa.

Carregar o executável do jogo (EBOOT descriptografado) na memória do PS3:

```powershell
.\build\mingw-release\sly4.exe --game PS3_GAME --no-window --dump-imports work\imports.csv
$env:SLY4_EBOOT = "$PWD\PS3_GAME\USRDIR\EBOOT.elf"; .\build\mingw-release\sly4_tests.exe
```

Requisitos para rodar: Windows 10/11 e uma GPU com Vulkan 1.3 (o `vulkan-1.dll` vem com o driver).

Recompilador (Fase 2; ver [docs/RECOMP.md](docs/RECOMP.md)). Ele gera o C++ do jogo a partir do seu `EBOOT.elf`. O próximo build compila esse código e o teste diferencial:

```powershell
.\build\mingw-release\sly4_recomp.exe emit --eboot PS3_GAME\USRDIR\EBOOT.elf --out-dir generated\ppu
.\docker\build.ps1
.\build\mingw-release\sly4_gendiff.exe --eboot PS3_GAME\USRDIR\EBOOT.elf --cases 8
```

Com o código gerado no build, o `sly4.exe` roda o jogo (Fase 3; ver [docs/KERNEL.md](docs/KERNEL.md)):

```powershell
.\build\mingw-release\sly4.exe --game PS3_GAME --no-window --seconds 20
```

Com o firmware do PS3 instalado no RPCS3, o SPURS do firmware roda como está (Fase 4; ver [docs/SPU.md](docs/SPU.md), 4c). O `libsre.prx` descriptografado vai em `work\firmware`, é recompilado para `generated\fw_libsre` e o jogo roda com `--firmware work\firmware`.

Estado atual: **Fases 0 a 4 concluídas; Fase 5 (GPU) com a saída alcançada: o jogo chega ao menu principal.**
- O EBOOT inteiro vira C++: 24.164 funções que compilam sem avisos.
- Comparado com o interpretador, o código gerado deu igual em todos os 98 mil casos.
- O interpretador foi validado antes contra o QEMU: 195 mil casos, 0 divergências.
- O jogo recompilado roda do ponto de entrada até o `cellGcmInit`. No caminho ele:
  - inicializa memória e `cellGame`, com o `PARAM.SFO` real;
  - cria 10 threads;
  - sobe o sistema de arquivos da Sony (FIOS) e monta o cache;
  - lê o `Sly4.psarc`, com a descompressão que no PS3 roda no SPU (EDGE zlib) feita em código nativo.
- Fase 4 (ver [docs/SPU.md](docs/SPU.md)):
  - o SPU tem decodificador, interpretador e gerador de C++ validados (as 266.842 palavras dos programas SPU iguais ao `objdump`; 925 mil casos do diferencial iguais ao interpretador);
  - com o firmware, o SPURS real roda em 6 SPUs, quase todo em código compilado, com interrupções e listas de DMA com parada;
  - os 2.577 blocos que a tarefa EDGE zlib descomprime no SPU durante o boot são idênticos ao inflate nativo, que bate com o `zlib` do Python nos 148.603 blocos do psarc.
- Fase 5, início: um `cellGcm` mínimo com um RSX que consome o FIFO sem desenhar, mais `cellAudio` e `cellPad` mínimos. O jogo sobe até o vídeo de abertura, com as threads de áudio (SCREAM, Bink) e de física (Havok, BFX) rodando.
- Fase 5 (ver [docs/GPU.md](docs/GPU.md)): o RSX tem rastreador de estado e um renderizador Vulkan que traduz os *shaders* do jogo direto para SPIR-V (os 541 traduzem). Tela de carregamento, splash da Sanzaru, tela de título e menu principal saem corretos (`--no-window --render --dump-frames pasta` grava os quadros em PNG; `--press start@40` aperta botões).
- O título e o menu batem com o RPCS3 (Fase 5 concluída).
- Fase 6 (ver [docs/FASE6.md](docs/FASE6.md)): o jogo tem som (música e efeitos pelo dispositivo padrão), controle (gamepad, ou o teclado na janela: setas, Enter = Start, X = Cross, C = Circle), saves (o idioma é pedido na primeira partida) e a abertura em vídeo Bink. O menu roda a ~37–41 quadros/s, agora com o pós-processamento que o jogo faz nas SPUs.
- Fase 7 (ver [docs/FASE7.md](docs/FASE7.md)): **a primeira fase é jogável.** Novo Jogo leva ao Prólogo em Paris. A fase carrega, a cena de abertura roda e o Sly anda, corre e pula pela rua até o museu, com física e câmera, a 30–34 quadros/s na janela, com som limpo e o controle (gamepad ou teclado). Duas correções liberaram a fase: as comparações de ponto flutuante da SPU (o carregamento travava na física) e o *clear* parcial de canais no renderizador (a cena apagava). Os pontos brancos nas bordas, do pós-processamento nas SPUs, eram a estimativa de recíproco (`frest`/`fi`) exata demais. Próximo passo: a Fase 8 (do POC ao jogo completo).
- O programa abre a janela Vulkan 1.3 na GPU dedicada.
- Reserva os 4 GiB do espaço de endereços do PS3 e carrega o `EBOOT.elf`: segmentos, TLS, parâmetros do processo e as 251 importações, idênticas às do `eboot_scan.py`.
- A CI (`.github/workflows/build.yml`) compila o `.exe`, roda os testes num runner Windows e publica os binários como artefato.

## Ambiente

Tudo roda em **Docker**; no Windows só são necessários Docker e Git.

| Imagem | Para quê |
|---|---|
| `sly4-mingw:1` | MinGW-w64 GCC 14, CMake, Ninja, headers do Vulkan e SDL3 estático: compila o `sly4.exe` (`docker\build.ps1`) |
| `sly4-tools:1` | Python 3.11 + capstone + Pillow: todas as ferramentas (`tools\run.ps1`) |
| `eclipse-temurin:21-jdk` | Ghidra sem interface (`tools\run_ghidra.ps1`) |
| `sly4-spu:1` | binutils 2.35 para SPU (`tools/spu/Dockerfile`) |
| `sly4-ppc:1` | binutils 2.35 para PowerPC com as instruções do Cell (`tools/ppc/Dockerfile`) |
| `sly4-ppc-oracle:1` | QEMU + GCC PowerPC64: oráculo do interpretador PPU (`tools/ppc/oracle`) |

Ferramentas externas, cada um baixa a sua:
- **RPCS3**: descriptografa o EBOOT e serve de referência durante o port;
- **Ghidra** 12.x.

Comece por [tools/README.md](tools/README.md).
