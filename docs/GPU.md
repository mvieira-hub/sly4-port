# RSX e cellGcm (Fase 5)

O RSX é a GPU do PS3. O jogo não fala com ela diretamente. Ele monta *command buffers* com a parte inline da `libgcm`, compilada dentro do EBOOT, num FIFO na memória principal mapeado no espaço de IO do RSX, e avança o registrador `put` à medida que escreve. A `cellGcmSys` do firmware cuida do resto: memória local, mapa de IO, *labels*, *reports*, *flip* e VBlank.

| Entrega | Estado |
|---|---|
| 5a. `cellGcm` mínimo e "RSX nulo": o FIFO é consumido sem desenhar, para o jogo seguir | **Concluída** (2026-10-22) |
| 5b. Rastreador de estado do RSX (registradores dos métodos NV4097) e módulo `runtime/rsx` | **Concluída** (2026-10-22) |
| 5c. *Vertex fetch*, texturas e *render targets* em Vulkan | **Concluída para o menu** (2026-10-22): superfícies coerentes com a memória, cópias 2D na GPU, *mipmaps*, *culling*, profundidade lida como textura e *shadow maps* |
| 5d. Tradutor de *shaders* VP/FP → SPIR-V | **Concluída para os 541 programas do jogo** (2026-10-22): desvios de VP; nenhum FP usa desvios ou laços |
| 5e. *Flip* para a *swapchain* e comparação com o RPCS3 | **Concluída** (2026-10-22): título e menu iguais aos do RPCS3, depois de três correções; TTY igual |

**Saída da fase (alcançada):** a splash da Sanzaru, a tela de título e o menu principal aparecem corretos, conferidos lado a lado com o RPCS3 (ver "Comparação com o RPCS3").

## 5a. cellGcm mínimo e RSX nulo

A 5a começou para destravar a saída da Fase 4: sem `cellGcm`, o jogo parava logo depois do `cellGcmInit` e nunca chegava às threads de áudio e física. Código em `runtime/kernel/hle_cell_gcm.cpp`, seguindo a `cellGcmSys.cpp` do RPCS3 (o FIFO, que ficava ali, foi para `runtime/rsx` na 5b).

**Inicialização (`_cellGcmInitBody`):**
- **Memória local:** 249 MiB em `0xC0000000`, como a `libgcm` informa ao jogo.
- **IO:** o `ioAddress` do jogo (8 MiB em `0x30B00000`) é mapeado no offset de IO 0. O `cellGcmMapMainMemory` mapeia outras áreas na primeira faixa livre, e o `cellGcmAddressToOffset` faz a tradução nos dois sentidos, em páginas de 1 MiB.
- **Buffer de comandos padrão:** dividido em fragmentos de 32 KiB. O primeiro pula 4 KiB reservados, e a última palavra de cada um fica para o salto.
- **Callback de contexto:** quando o fragmento enche, o código inline do jogo chama `CellGcmContextData::callback`. É uma função nativa, chamada por um descritor que o HLE agora sabe criar (`HleTable::callback`). Ela libera o fragmento atual (`put`), escreve um salto para o próximo e espera o RSX sair dele.
- **Estruturas no convidado:** o registrador de controle (`put`, `get`, `ref`), 256 *labels* e os *reports* (em `0x0E000000` da memória local).

**RSX nulo:** uma thread do host segue o FIFO de `get` até `put`:
- segue saltos (antigos e novos), chamadas e retornos;
- trata os métodos que o jogo observa: `SET_REFERENCE`, semáforos NV406E (inclusive a espera do `ACQUIRE`), semáforos NV4097 de *back end* e de textura, `GET_REPORT` (com *timestamp*) e o *flip* (`0xFEAC`), que só conta quadros;
- os demais métodos são ignorados: nada é desenhado ainda.

**VBlank:** uma thread de sistema conta VBlanks a 60 Hz e chama o *handler* do jogo (`cellGcmSetVBlankHandler`), como a *interrupt thread* do PS3.

## O que o jogo fez com isso

O jogo passa do `cellGcmInit`, cria a thread "Loading Screen", que vira quadros pelo FIFO (perto de 1.000 *flips* nos primeiros 2 minutos), e segue o boot até o primeiro vídeo (`Movies/SanzaruSplash.bik`, com as threads "Bink Snd" e "Bink IO"). No caminho vieram junto, de forma mínima, duas bibliotecas da Fase 6:
- **`cellAudio`** (`hle_cell_audio.cpp`): portas, *buffers* e o índice de leitura que o hardware avança a cada 256 amostras (5,33 ms). Os blocos são consumidos e zerados, sem tocar som ainda; o resumo mostra o pico do sinal.
- **`cellPad`** (`hle_cell_pad.cpp`): um DualShock 3 conectado na porta 0, parado. Sem ele o jogo abria o aviso "There are no wireless controllers connected!".

## 5b. Rastreador de estado

O RSX virou um módulo próprio, `runtime/rsx`:

| Arquivo | Conteúdo |
|---|---|
| `rsx.h`, `rsx_core.cpp` | Mapa de IO, *display buffers*, a thread do FIFO (saltos, chamadas, semáforos, *reports*, *flip*), as cópias 2D feitas na CPU (NV0039 memória a memória, NV308A imagem vinda da CPU, NV3089 cópia escalada) e a captura de quadros |
| `rsx_methods.h/.cpp` | Os *offsets* dos métodos (NV406E, NV4097 e as classes 2D nos *subchannels* 1–6) e seus nomes, para os logs |
| `rsx_state.h/.cpp` | O banco de registradores (cada escrita do FIFO), a memória do *vertex program* (512 instruções) e das constantes (468 vec4), os valores constantes de atributo, e decodificadores de superfície, *array* de vértices e textura |
| `rsx_backend.h` | O que vai para o renderizador: *clear*, *draw* (com os intervalos de vértices ou índices, ou os dados *inline*), *flip*, sincronização e invalidação de memória |

Os registradores de contexto DMA começam onde a `libgcm` espera (semáforos na área de *labels*, *reports* e superfícies na memória local). Sem isso, as liberações de semáforo não eram gravadas, e o motor do jogo imprimia o aviso próprio "RSX Potential Locks" com todos os *labels* em 0.

`sly4.exe ... --rsx-capture N,arquivo.txt` grava os comandos do quadro N, já com os nomes dos métodos. Foi a captura da tela de carregamento que guiou a 5c e a 5d:
- *clear* preto da superfície A8R8G8B8 de 1280×720 (o *display buffer* 0);
- um *triangle strip* de 4 vértices *inline* (posição, cor e coordenada de textura);
- um *vertex program* de 6 instruções e um *fragment program* em memória principal;
- uma textura A8R8G8B8 linear de 192×104.

## 5c–5e. Renderizador Vulkan

Código em `runtime/gpu` (`rsx_vk.cpp`, `rsx_vp.cpp`, `rsx_fp.cpp`, `spirv.cpp`, `rsx_texture.cpp`). O renderizador usa o mesmo dispositivo e a mesma fila da janela, com uma trava para as submissões.

**Superfícies.** Cada superfície do RSX (endereço, formato e tamanho) é uma imagem Vulkan, desenhada com *dynamic rendering*. A8R8G8B8 vira RGBA8, Z24S8 vira D24S8 (ou D32S8 onde não houver). Uma textura cujo endereço é o de uma superfície já desenhada usa a própria imagem.

***Shaders* (5d).** Os dois tradutores geram SPIR-V direto, com um construtor próprio (`spirv.h`), sem compilador de GLSL no meio:
- **Vertex program (NV40):** quatro palavras por instrução, com a operação vetorial e a escalar executadas juntas. A soma usa `src0 + src2` e as escalares usam `src2.x`. Há escrita mascarada e condicional pelos códigos de condição, saturação e registradores de endereço para constantes indexadas.
  - O formato dos campos foi conferido contra o *vertex program* real do jogo.
  - A transformação de *viewport* do RSX (escala e deslocamento) é feita no próprio *shader*, e o *viewport* do Vulkan cobre a superfície inteira.
  - **Desvios** (BRA, BRI, BRB, CAL, CLI, CLB, RET), como no RPCS3: um programa que os tenha vira um laço sobre `switch (pc)`, com um `case` por instrução.
    - Cada desvio escolhe o próximo `pc` com `OpSelect`: pelos códigos de condição, ou, em BRB/CLB, pelo bit `SET_TRANSFORM_BRANCH_BITS` (que vai no *uniform buffer*).
    - CAL/CLB empilham o retorno numa pilha de 8 níveis, e RET desempilha ou termina o programa.
    - Um contador encerra laços sem fim.
    - Sem desvios, o programa continua linear.
- **Fragment program (NV40):** palavras com as metades trocadas e constantes *inline* depois da instrução que as usa.
  - Essas constantes são lidas da memória a cada *draw*, porque o jogo as remenda no lugar, e vão num *uniform buffer*; o código traduzido é guardado pelo *hash* das instruções sem as constantes.
  - Cobre registradores R e H, códigos de condição, execução condicional, escala, saturação, texturas 2D/3D/cubo, KIL, FENCT/FENCB e o *alpha test* do RSX.
  - Não traduz desvios nem laços, mas nenhum FP do jogo os usa.
- **Os 541 programas do jogo** (`Shaders.Zip`, extraídos por `tools/cgbin_info.py` para `work/shaders`) passam todos pelos tradutores: o teste `rsx_game_shaders_translate`, com `SLY4_SHADERS=work\shaders`, dá **326 VPs e 215 FPs, nenhum sem tradução**.

**Vértices.** Cada atributo é convertido para float4 na CPU, a partir dos formatos do RSX (float, half, inteiros normalizados, *bytes*, CMP), e um atributo sem *array* usa o valor constante. Os índices de 16 e 32 bits viram 32 bits. Os primitivos que o Vulkan não tem (quads, *line loop*) viram listas de índices.

**Texturas.** Os formatos de 8, 16 e 32 bits são decodificados na CPU (linear ou *swizzled*), com o *remap* do registrador CONTROL1, para RGBA8. DXT1/3/5 vão direto como BC1/2/3, com o *remap* no *swizzle* da *view*.
- **Mipmaps:** todos os níveis que o formato declara, um depois do outro na memória. DXT e lineares mantêm o *pitch*; as *swizzled* têm cada nível com seu próprio *swizzle*. O *sampler* usa o filtro de *mipmap* do registrador FILTER.
- **Detecção de mudança:** uma textura é reenviada quando o *hash* dos seus bytes muda. O *hash* é feito no máximo uma vez por quadro, de 8 em 8 bytes; as escritas do próprio RSX na memória o refazem. Antes, o FNV byte a byte a cada *draw* era 40% da thread do RSX no menu.

**Superfícies e memória (coerência).** O que a GPU desenha e o que a CPU escreve ficam coerentes nos dois sentidos:
- uma superfície desenhada fica marcada `gpu_dirty`. Antes de o RSX ler ou sobrescrever aquela memória com a CPU (cópias 2D), ela é lida de volta (`flush`, só para os formatos de 32 bits);
- uma superfície em que a CPU escreveu fica `cpu_dirty` e é reenviada antes de a GPU usá-la de novo (como alvo, textura ou no *flip*).
- **Cópias escaladas NV3089 na GPU:** quando a origem é uma superfície, a cópia vira um `vkCmdBlitImage`, sem passar pela memória.
  - A superfície pode conter o endereço em vez de começar nele: o NV3089 copia faixas de no máximo 1024 colunas.
  - Um destino na memória principal ganha uma superfície-sombra. O jogo guarda o *display buffer* em `0x31E00000` e o copia de volta a cada quadro.
  - Sem isso, do vídeo em diante a imagem saía toda preta: as cópias liam memória que nunca recebia o que a GPU desenhara.
- Uma textura menor que a superfície no mesmo endereço amostra só o retângulo dela (uma cópia), não a superfície inteira.

**Estado fixo.** *Blend*, máscara de cor, profundidade, *stencil* e *culling* entram na chave do *pipeline*. *Viewport*, *scissor*, cor de *blend* e referência de *stencil* são dinâmicos.
- **Culling:** SET_CULL_FACE e SET_FRONT_FACE, sem espelhar a orientação. Conferido nos dois sentidos: com a orientação invertida, a van e o logo da tela de título saem do avesso.

***Flip* (5e).** No *flip*, a superfície do *display buffer* é copiada para uma imagem "da frente" (se nada foi desenhado ali, usa a memória que a CPU deixou). A janela mostra essa imagem escalada, mantendo a proporção. A *swapchain* é UNORM, porque o PS3 já entrega valores com gama.

**Salto para si mesmo no FIFO.** A `libgcm` faz o RSX "esperar" saltando para o próprio endereço até o PPU reescrever aquela palavra. A thread do RSX girava nesse salto a toda velocidade: eram bilhões de "comandos" e um núcleo do host ocupado. Agora ela espera 20 µs por volta, e o resumo conta essas esperas à parte.

**Sem janela.** `sly4.exe ... --no-window --render` renderiza com o Vulkan sem janela, e `--dump-frames pasta,N` grava um PNG a cada N quadros (`core/png.h`). Foi assim que a primeira imagem foi conferida: a tela de carregamento com a máscara do Sly, correta, com todos os 547 *draws* dos primeiros 40 s desenhados, 6 programas traduzidos e 4 *pipelines*.

**Com janela.** O caminho com janela também foi conferido: *swapchain* 1280×720 em FIFO, o jogo rodando sem erro. A janela não abria por um `||` no lugar de `|` nos *flags* do `SDL_CreateWindow`.

**Testes** (`test_rsx.cpp`):
- a tradução do *vertex program* real do jogo e de um com desvio (laço com `switch`);
- um *fragment program* com constante *inline*;
- a decodificação de textura (linear, *swizzled*, *remap*) e o *layout* dos *mipmaps*;
- os decodificadores de estado;
- com `SLY4_SHADERS`, os 541 *shaders* do jogo.

## O boot até o vídeo e depois dele

Três correções levaram o jogo do carregamento ao vídeo de abertura e além:
- **A trava do boot nas tarefas do Havok** (em 1 a cada 3 execuções, mais ou menos) era um `putllc` do SPU apagando uma escrita comum do PPU na mesma linha de 128 bytes. Detalhes em [SPU.md](SPU.md), item 4 da 4f. Depois da correção: 10 de 10 execuções sem travar.
- **O vídeo do Bink andava a 2,6 quadros/s.** O perfilador por amostragem (`--ppu-profile AglRenderThread`) mostrou o tempo no `feclearexcept` do MinGW. Ele executa `CPUID` a cada chamada, e o núcleo de ponto flutuante do PPU (`cpu/ppu_fpu.cpp`) o chamava a cada operação. Sob o Hyper-V (Docker/WSL2), `CPUID` é interceptado pelo hipervisor. Agora o modo de arredondamento e os flags vão direto no MXCSR. Validado contra o QEMU: 195.000 casos, 0 divergências. O vídeo passou a rodar a ~60 *flips*/s.
- **Os SPUs do SPURS paravam com "instrução inválida"** ao escrever no canal 69 (`SPU_Set_Bkmk_Tag`, marcador do monitor de desempenho). Os canais 69–71 agora são aceitos e ignorados. Sem isso, o *job* morto deixava o RSX esperando um semáforo para sempre, e o jogo imprimia o aviso próprio "RSX Potential Locks".

Também: o tradutor de *fragment program* aceita FENCT/FENCB (barreiras, sem cálculo), o que reduziu os *draws* descartados de 42.915 para 750.

## Até o menu principal

Com a coerência entre superfícies e memória, o jogo mostra, em ordem:
1. a tela de carregamento;
2. "Sony Computer Entertainment presents";
3. o vídeo da Sanzaru (os três macacos e o logo);
4. a tela de título com a van em 3D e "Press START button";
5. o **menu principal** (New Game, Load Game, Cross-Save, Options, Extras).

Para passar do título sem janela, `--press start@40,start@42` aperta botões do controle em segundos dados da execução, cada aperto por 250 ms. É um roteiro de teste; a entrada de verdade é a Fase 6.

```powershell
.\build\mingw-release\sly4.exe --game PS3_GAME --no-window --render --dump-frames work\frames,120 --seconds 100 --press start@40,start@42 --firmware work\firmware
```

No menu: nenhum *draw* descartado, 41 programas e 39 *pipelines*.

O código SPU novo do vídeo e do menu foi gravado na cache SPU e compilado ([SPU.md](SPU.md), 4e). Eram 15.430 blocos que rodavam no interpretador; agora são 45.674 blocos compilados.

### O que falta

- **Desempenho:** o menu roda a ~41–47 quadros/s (ver abaixo), acima dos 30 que o jogo mira.
- MRT, desvios e laços em *fragment programs*, e superfícies de 16 bits e de ponto flutuante na coerência com a memória: nada disso aparece até o menu.

## Comparação com o RPCS3 (5e)

O RPCS3 local (v0.0.43, com o firmware do disco) roda o jogo numa janela. O `tools/rpcs3_capture.ps1` aperta as teclas nos segundos pedidos e tira capturas com o F12. Cada tecla fica apertada por 150 ms, porque o RPCS3 lê o teclado uma vez por milissegundo e não via o aperto instantâneo do `SendKeys`. O jogador 1 usa o teclado (`config\input_configs\global\Default.yml`: Enter = Start, X = Cross). Os mesmos pontos saem do sly4 com `--dump-frames`, e `work/comparacao_5e.png` põe os dois lado a lado.

**Uma diferença esperada.** No RPCS3, o jogo pede o idioma antes do título. O `cellSaveDataAutoLoad2` real chama as funções do jogo, e ele vê que não há save (primeira partida). No sly4, as funções de save ainda são *stubs* que não chamam ninguém, e o jogo pula essa tela. Os saves estão na Fase 8.

**O que a comparação achou.** Interface, logo e textos eram iguais nos dois. O fundo não: no sly4, o túnel do tempo saía escuro, borrado e coberto de feixes radiais de luz. A captura do quadro do menu mostrou as passadas:
1. a cena (túnel, van) no alvo principal, com profundidade Z24S8 em `0x00010000`;
2. dois *shadow maps* Z16 de 1024×1024, empilhados numa superfície de 1024×2048 em `0x020A0000`;
3. o resto da cena e o pós-processamento (reduções a 640×360 e 320×180, raios de luz, composição). Eles leem a profundidade principal como **A8R8G8B8** e os *shadow maps* como **DEPTH16** com comparação (`zfunc` em `TEXTURE_ADDRESS`) e como **G8B8**.

Três correções:

| Problema | Correção |
|---|---|
| Profundidade lida como textura vinha da memória, onde nunca era escrita: o efeito de luz calculava com lixo | A profundidade vira cor **na GPU**: o plano de profundidade vai para um *buffer* e dele para uma imagem RGBA8 (D24S8) ou RG8 (D16). O *swizzle* da *view* põe cada byte onde a decodificação da memória do PS3 poria (palavras *big-endian*, profundidade nos 24 bits altos). O *remap* do `control1` vem por cima. O *stencil* (B do A8R8G8B8) lê 0 |
| `DEPTH16` com comparação não era decodificado (textura vazia) | *Shadow maps* amostrados direto da superfície D16, ou de uma cópia do pedaço em que a textura começa. *Sampler* com comparação, com a função invertida como no RPCS3 (`get_compare_func(op, true)`). No *shader*, imagem de profundidade com `OpImageSampleDrefImplicitLod` (e a versão projetiva no `TXP`): o `z` é a referência |
| *Sampler* ignorava o LOD mínimo/máximo (`CONTROL0`) e o *bias* (`FILTER`) | Lidos como o RPCS3 os lê. A maioria das texturas do menu tem LOD máximo 0 |

Além disso, uma superfície de profundidade passa a ser escrita na memória do convidado quando alguém lê aquela memória: `flush` em *big-endian*, D24S8 com o *stencil* no byte baixo, ou D16. O *buffer* dessa leitura de volta agora usa memória com *cache* do host. Sem *cache*, cada byte lido pela CPU atravessava o barramento, e uma profundidade de 1280×720 levava ~0,2 s. Com a conversão na GPU, o menu voltou a ~41–47 quadros/s, com 1 leitura de volta em 100 s (eram ~3 por quadro na primeira versão, que passava pela CPU: ~24 quadros/s).

**TTY.** Até o menu, 172 das 178 linhas do RPCS3 são iguais e na mesma ordem. O que muda são os números de "Shader patch buffer: N k free", que dependem de quantos *shaders* o jogo remenda por quadro, e uma linha que o jogo imprime em dois pedaços.

## Desempenho do menu: de ~2 para ~41–47 quadros/s

O critério foi medir antes de mexer. As ferramentas desta etapa:
- `--call-times`: tempo dentro de cada chamada HLE e de sistema, por thread e por local de chamada;
- `--ppu-profile NOME`: amostragem de onde uma thread roda no `sly4.exe`, também para `RSX` e `SPU1`–`SPU6`, com a parte fora do executável separada por DLL;
- `--rsx-timeline`: as esperas longas do RSX, os *labels* e os *flips*, com o mesmo relógio absoluto do `--watch-line`, e cada SPU que acordou depois de mais de 1 ms parada;
- `--thread-timeline NOME`: as chamadas da thread PPU `NOME` com tempo absoluto (as de mesmo local em sequência viram uma linha);
- no resumo: o tempo da thread do RSX por categoria, os saltos para si mais esperados e o tempo de cada SPU esperando em canais.

O caminho de um quadro no menu:
1. o `main_thread` lança *jobs* SPU (geometria EDGE e outros) e espera por eles com `usleep(300)` em laço;
2. os *jobs* escrevem comandos do RSX e desfazem os saltos para si em que o RSX espera;
3. o RSX escreve o *label* 0x40, que o `main_thread` espera para começar o próximo quadro.

O que limitava, em ordem:

| Medida | Antes | Depois |
|---|---|---|
| *Hash* de texturas a cada *draw* (FNV byte a byte, 40% do RSX) | uma vez por quadro, 8 bytes por vez | ~2 → ~8 quadros/s, junto com as cópias 2D na GPU |
| Leitura da GPU para a memória a cada cópia NV3089 (2 por quadro) | cópia na GPU, com superfície-sombra na memória principal | (idem) |
| Espera pela GPU em cada liberação de semáforo (~23 por quadro) | sem espera: os dados já foram copiados no *draw* | — |
| `sys_timer_usleep` acordando 0,2–0,3 ms atrasado | espera curta termina com *spin* até o prazo | — |
| **Semânticas SPU fora de linha** nos blocos gerados (44% de um SPU) | `always_inline` | inflate 7,3 → 6,5 s |
| `shufb` byte a byte, *load*/*store* de quadword byte a byte, `ftz` com desvios | `pshufb` (SSE4.1), duas inversões de 64 bits, `ftz` sem desvios | inflate 6,5 → 3,9 s; **menu ~8,6 → ~23 quadros/s** |
| `memcmp` do código de cada bloco SPU a cada entrada (~10% de um SPU) | cache de blocos por SPU, validado por épocas das páginas de 4 KiB do *local store* | menu ~23 → ~26–28 quadros/s, com os demais desta linha |
| DMA com `memcpy` (o RSX via às vezes um comando de salto escrito pela metade) | `dma_copy` com *stores* alinhados de 16 bytes; o RSX tenta de novo um salto inválido | (correção) |
| Filtro de denormais e NaN em cada operando e resultado de `fm`/`fma` (~25 instruções x86 cada) | DAZ/FTZ do MXCSR em volta do código SPU (`SpuFloatMode`); só o NaN do resultado é normalizado; precisão dupla continua IEEE (`IeeeDoubles`) | **menu ~26–28 → ~41–47 quadros/s**, com os três abaixo |
| Laços elemento a elemento na ordem do convidado: o GCC invertia as *lanes* antes e depois de cada `ori`, `ai`, `fm`... | laços do último elemento ao primeiro (a ordem da memória do host) | (idem) |
| Lógica de 128 bits (`and`, `selb`...) e deslocamentos por bytes (`shlqbyi`, `rotqby`...) em pares de registradores inteiros | SSE, e `pshufb` com máscaras de uma tabela | (idem) |
| *Load*/*store* de quadword com duas inversões de 64 bits; busca do bloco seguinte numa chamada | um `pshufb`; acerto do cache de blocos sem chamada | (idem) |

Dois cuidados que vieram junto:
- **Memória da compilação.** Com o *inline* forçado, cada arquivo de blocos SPU precisa de alguns GiB para compilar. Um *pool* do Ninja limita esses arquivos a 4 por vez (`CMakeLists.txt`); o *build* completo leva ~30 min.
- **Aliasing.** Com tudo *inline*, o diferencial SPU achou 123 casos errados. A causa: o `SpuReg` é uma *union* lida e escrita por referências a membros de tipos diferentes (`u32()`/`f32()`), e com *strict aliasing* o GCC supôs que não se sobrepunham. Um `fnms` lia um registrador de antes do `iohl` que o completava. O projeto agora compila com `-fno-strict-aliasing`, e o diferencial voltou a dar 700.328 casos iguais.

Mais dois cuidados desta última rodada:
- **`x * 1.0` some.** Quando o `1.0` (ou `-1.0`) vem de um `ilhu` do mesmo bloco, o GCC troca a multiplicação por `x` (ou `-x`), e um operando denormal passa sem virar zero. O diferencial pegou 25 casos. `-fsignaling-nans` proíbe essa troca, nos blocos gerados e no interpretador.
- **Onde o modo vale.** `spu_run`, `spu_run_blocks`, o `sly4_spudiff`, o oráculo e os testes das semânticas ligam o `SpuFloatMode`. O diferencial (700 mil casos, com registradores aleatórios, cheios de denormais e NaN) e o oráculo continuam iguais.

Como estava antes desta rodada (~26–28 quadros/s), cada SPU passava ~45% do tempo parada esperando. O quadro era limitado pela cadeia de *jobs*, um depois do outro, e não pela soma do trabalho. Por isso acelerar cada instrução SPU encurtou o quadro quase na mesma proporção.
