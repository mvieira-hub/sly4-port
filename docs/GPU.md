# RSX e cellGcm (Fase 5)

O RSX é a GPU do PS3. O jogo não fala com ela diretamente. Ele monta *command buffers* com a parte inline da `libgcm`, compilada dentro do EBOOT, num FIFO na memória principal mapeado no espaço de IO do RSX, e avança o registrador `put` à medida que escreve. A `cellGcmSys` do firmware cuida do resto: memória local, mapa de IO, *labels*, *reports*, *flip* e VBlank.

| Entrega | Estado |
|---|---|
| 5a. `cellGcm` mínimo e "RSX nulo": o FIFO é consumido sem desenhar, para o jogo seguir | **Concluída** (2026-10-22) |
| 5b. Rastreador de estado do RSX (registradores dos métodos NV4097) e módulo `runtime/rsx` | **Concluída** (2026-10-22) |
| 5c. *Vertex fetch*, texturas e *render targets* em Vulkan | Em andamento: o básico funciona (tela de carregamento) |
| 5d. Tradutor de *shaders* VP/FP → SPIR-V | Em andamento: sem desvios nem laços |
| 5e. *Flip* para a *swapchain* e comparação com o RPCS3 | Em andamento: *flip* e quadros em PNG funcionam |

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
- **Fragment program (NV40):** palavras com as metades trocadas e constantes *inline* depois da instrução que as usa.
  - Essas constantes são lidas da memória a cada *draw*, porque o jogo as remenda no lugar, e vão num *uniform buffer*; o código traduzido é guardado pelo *hash* das instruções sem as constantes.
  - Cobre registradores R e H, códigos de condição, execução condicional, escala, saturação, texturas 2D/3D/cubo, KIL e o *alpha test* do RSX.
  - Ainda não traduz desvios nem laços; um programa que os use é deixado de lado, com aviso.

**Vértices.** Cada atributo é convertido para float4 na CPU, a partir dos formatos do RSX (float, half, inteiros normalizados, *bytes*, CMP), e um atributo sem *array* usa o valor constante. Os índices de 16 e 32 bits viram 32 bits. Os primitivos que o Vulkan não tem (quads, *line loop*) viram listas de índices.

**Texturas.** Os formatos de 8, 16 e 32 bits são decodificados na CPU (linear ou *swizzled*), com o *remap* do registrador CONTROL1, para RGBA8. DXT1/3/5 vão direto como BC1/2/3, com o *remap* no *swizzle* da *view*. Uma textura é reenviada quando o *hash* dos seus bytes muda. Por enquanto só o nível 0.

**Estado fixo.** *Blend*, máscara de cor, profundidade e *stencil* entram na chave do *pipeline*. *Viewport*, *scissor*, cor de *blend* e referência de *stencil* são dinâmicos. O *culling* fica desligado até a orientação das faces ser conferida.

***Flip* (5e).** No *flip*, a superfície do *display buffer* (ou, se nada foi desenhado ali, a memória que a CPU deixou) é copiada para uma imagem "da frente". A janela mostra essa imagem escalada, mantendo a proporção. A *swapchain* agora é UNORM, porque o PS3 já entrega valores com gama.

**Sem janela.** `sly4.exe ... --no-window --render` renderiza com o Vulkan sem janela, e `--dump-frames pasta,N` grava um PNG a cada N quadros (`core/png.h`). Foi assim que a primeira imagem foi conferida: a tela de carregamento com a máscara do Sly, correta, com todos os 547 *draws* dos primeiros 40 s desenhados, 6 programas traduzidos e 4 *pipelines*.

**Testes** (`test_rsx.cpp`): a tradução do *vertex program* real do jogo, um *fragment program* com constante *inline*, a decodificação de textura (linear, *swizzled* e *remap*) e os decodificadores de estado.

## O boot até o vídeo e depois dele

Três correções levaram o jogo do carregamento ao vídeo de abertura e além:
- **A trava do boot nas tarefas do Havok** (em 1 a cada 3 execuções, mais ou menos) era um `putllc` do SPU apagando uma escrita comum do PPU na mesma linha de 128 bytes. Detalhes em [SPU.md](SPU.md), item 4 da 4f. Depois da correção: 10 de 10 execuções sem travar.
- **O vídeo do Bink andava a 2,6 quadros/s.** O perfilador por amostragem (`--ppu-profile AglRenderThread`) mostrou o tempo no `feclearexcept` do MinGW. Ele executa `CPUID` a cada chamada, e o núcleo de ponto flutuante do PPU (`cpu/ppu_fpu.cpp`) o chamava a cada operação. Sob o Hyper-V (Docker/WSL2), `CPUID` é interceptado pelo hipervisor. Agora o modo de arredondamento e os flags vão direto no MXCSR. Validado contra o QEMU: 195.000 casos, 0 divergências. O vídeo passou a rodar a ~60 *flips*/s.
- **Os SPUs do SPURS paravam com "instrução inválida"** ao escrever no canal 69 (`SPU_Set_Bkmk_Tag`, marcador do monitor de desempenho). Os canais 69–71 agora são aceitos e ignorados. Sem isso, o *job* morto deixava o RSX esperando um semáforo para sempre, e o jogo imprimia o aviso próprio "RSX Potential Locks".

Também: o tradutor de *fragment program* aceita FENCT/FENCB (barreiras, sem cálculo), o que reduziu os *draws* descartados de 42.915 para 750.

Em 120 s, o jogo passa do vídeo da Sanzaru e desenha cenas 3D: 336 mil *draws*, 306 milhões de vértices, 39 programas traduzidos.

### O que falta

- **Do vídeo em diante a imagem sai preta.** A hipótese mais forte: a imagem final chega ao *display buffer* por cópias escaladas NV3089, quase uma por *flip*. Essas cópias rodam na CPU e leem a origem da memória do convidado, mas a origem é uma superfície desenhada no Vulkan, que nunca volta para a memória. Falta tornar as cópias 2D coerentes com o cache de superfícies (fazer a cópia na GPU quando a origem for uma superfície, ou ler a superfície de volta antes).
- O FIFO do RSX conta bilhões de "comandos" num quadro: algum laço (provavelmente a espera de um semáforo) conta voltas como comandos. Conferir.
- *Vertex programs* com desvios (operação escalar 0x11, BRB) e os desvios e laços nos *fragment programs*; *mipmaps*, MRT, *culling* e a comparação quadro a quadro com o RPCS3.
- A apresentação na janela foi escrita, mas só o caminho sem janela foi conferido (pelos PNGs).
