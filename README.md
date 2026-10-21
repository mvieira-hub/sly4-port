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

## Ambiente

Tudo roda em **Docker**; no Windows só são necessários Docker e Git.

| Imagem | Para quê |
|---|---|
| `sly4-tools:1` | Python 3.11 + capstone + Pillow: todas as ferramentas (`tools\run.ps1`) |
| `eclipse-temurin:21-jdk` | Ghidra sem interface (`tools\run_ghidra.ps1`) |
| `sly4-spu:1` | binutils 2.35 para SPU (`tools/spu/Dockerfile`) |
| `sly4-ppc:1` | binutils 2.35 para PowerPC com as instruções do Cell (`tools/ppc/Dockerfile`) |

Ferramentas externas, cada um baixa a sua:
- **RPCS3**: descriptografa o EBOOT e serve de referência durante o port;
- **Ghidra** 12.x.

Comece por [tools/README.md](tools/README.md).
