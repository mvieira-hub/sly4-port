# Áudio, controle e vídeo (Fase 6)

| Entrega | Estado |
|---|---|
| 6a. Saves (`cellSaveData`), que o som exigiu | **Concluída** (2026-10-23) |
| 6b. Som: as portas do `cellAudio` saindo pelo dispositivo padrão | **Concluída** (2026-10-23) |
| 6c. Controle: `cellPad` ligado ao gamepad e ao teclado da janela | **Concluída** (2026-10-23); testado com o teclado |
| 6d. Vídeo Bink: a abertura da Sanzaru na tela | **Concluída** (2026-10-23) |

**Saída da fase (alcançada):** o menu tem música e efeitos, a navegação funciona pelo controle (o teclado na janela; o gamepad pelo mesmo caminho) e a abertura em vídeo aparece certa.

## 6a. O silêncio vinha dos saves

Em todas as rodadas até aqui, o resumo dizia "pico 0.000": o mixer do jogo (BRB/SCREAM, nas SPUs) escrevia blocos de zeros nas portas do `cellAudio`. A música era lida do psarc. O `--fs-reads` (novo) mostrou 358 leituras de `Audio/Stream/Music/Global/99_Front_End.xvag`. Mesmo assim, saía silêncio.

A causa estava longe do áudio. O `cellSaveDataAutoLoad2` era um *stub*: devolvia `CELL_OK` sem chamar as funções do jogo. O jogo tomava o buffer intocado como as configurações salvas, com idioma 0 e **volumes de música e efeitos 0**. No RPCS3, a mesma chamada responde que não há save. O jogo então cria os padrões e pede o idioma, uma tela que o sly4 pulava (ver GPU.md, "Comparação com o RPCS3").

`runtime/kernel/hle_cell_savedata.cpp` segue o `savedata_op` do RPCS3 (`cellSaveData.cpp`), sem os diálogos:
- **Escolha do diretório.** Os saves ficam em `/dev_hdd0/home/00000001/savedata/<dir>`. A função de lista ou de diretório fixo escolhe o diretório. Numa lista vale a escolha do próprio jogo (o foco, os dados novos ou o primeiro da lista fixa).
- **Função de estado.** Recebe o `PARAM.SFO` e a lista de arquivos e diz o que gravar no `PARAM.SFO`. Para dados que não existem e sem `setParam`, a resposta é `CELL_SAVEDATA_ERROR_PARAM`, como o firmware ("sysutil savedata parameter error : 50"), e o jogo segue com os padrões.
- **Função de arquivos.** Lê, escreve e apaga um arquivo por vez. O diretório é gravado por inteiro no fim: numa pasta `.working_` e depois renomeado.
- **Funções.** `cellSaveDataAutoLoad2/AutoSave2`, `ListLoad2/ListSave2`, `FixedLoad2/FixedSave2`, as versões sem 2 e o `Delete2`. O `Delete2` responde que o usuário saiu do diálogo.

Na primeira partida o jogo pede o idioma e grava `BCUS98247_SETTINGS` com `ICON0.PNG` (75.003 bytes) e `SYS_DATA` (36 bytes), os mesmos tamanhos do RPCS3. Daí em diante, o pico do som é ~0,3. A pasta de dados é `%LOCALAPPDATA%\sly4-port`, ou a de `--data`.

## 6b. Som

O relógio do `cellAudio` continua no timer do host: um bloco de 256 amostras a cada 5,33 ms. A cada bloco:
- as portas em execução são misturadas em estéreo. As de 8 canais (L, R, C, LFE e os dois pares de surround) viram 2, com o central e os surround a −3 dB; o LFE fica de fora;
- o resultado vai para o dispositivo padrão pelo SDL3 (WASAPI no Windows), em float de 48 kHz;
- o relógio do dispositivo deriva do timer: com mais de 100 ms esperando na fila, um bloco é descartado; com a fila vazia, toca silêncio.

| Opção | O que faz |
|---|---|
| `--no-audio` | sem saída de som; o relógio do jogo continua |
| `--dump-audio arquivo.wav` | grava também a mistura em WAV (16 bits, estéreo, 48 kHz), para conferir sem ouvir |

**Medido no menu:** a música entra a ~−27 dB RMS, em estéreo de verdade (os canais diferem). Não houve nenhum salto acima de meia escala entre amostras. De 9.562 blocos, 10 vieram vazios (0,1%), nas transições do start. Na janela: "som em Fones de ouvido (...)", pico 0,396, 0 blocos descartados.

## 6c. Controle

`app/pad_input.cpp` lê, a cada quadro da janela, o primeiro gamepad conectado (o SDL conhece DualShock, DualSense, Xbox...) e o teclado quando a janela tem o foco. O resultado vai para o `cellPad` (`kernel/hle_cell_pad.cpp`), que junta duas fontes:
- os dispositivos;
- o roteiro do `--press`.

Um botão conta como apertado se estiver apertado em qualquer uma. Vale a pressão maior e o analógico que saiu do centro. Gatilhos são analógicos (pressão; botão a partir de um quarto do curso). A vibração que o jogo pede (`cellPadSetActDirect`) volta para o gamepad.

| Teclado | Controle |
|---|---|
| setas | direcional |
| W A S D | analógico esquerdo |
| I J K L | analógico direito |
| Enter / Espaço | Start / Select |
| X, C, Z, V | Cross, Circle, Square, Triangle |
| Q / E, R / T, F / G | L1 / R1, L2 / R2, L3 / R3 |

Testado na janela com teclas enviadas pelo Windows: Enter leva do título ao menu e duas setas para baixo levam a seleção a "Cross-Save". O gamepad usa o mesmo caminho, mas não havia um conectado para conferir.

## 6d. O vídeo Bink saía verde

A abertura (`Movies/SanzaruSplash.bik`) saía como uma tela verde lisa por ~4,5 s. Verde é o que sai da conversão YUV→RGB quando os três planos são zero. A investigação, em ordem:
1. **Os descritores eram lixo.** Os registradores das três texturas (Y, U, V) vinham com o padrão `FF000000` (offset `0xFF000000`, formato `0xFF`): um descritor `CellGcmTexture` sobrescrito por pixels pretos opacos.
2. **Onde ficam os descritores.** Instrumentando o `cellGcmSetTexture` inline do jogo e o objeto `BINKFRAMEBUFFERS`, apareceu o layout: o jogo põe o descritor de cada plano logo depois dos pixels do plano. O plano Y em memória estava perfeito.
3. **Quem escrevia o preto.** A nova opção `--watch-write` (página só de leitura; cada escrita é registrada com a thread e o código do host) apontou o próprio sly4: o `flush` do renderizador. Uma superfície RGBA 1280×720 em `0x31E00000`, desenhada pelo RSX antes do vídeo, ainda estava marcada como "desenhada e não escrita na memória". O jogo tinha liberado aquela memória e entregue ao Bink. Quando o RSX foi ler a textura do Bink, o `flush` escreveu a superfície velha por cima dos planos.

No PS3 o RSX escreve direto na memória, então isso não acontece. O emulador precisa saber quando a CPU escreve sobre uma superfície que só existe na GPU. O RPCS3 faz isso com proteção de página, "invalidate on write", e o sly4 agora faz o mesmo:
- **Proteção.** Toda superfície cuja cópia na GPU está em dia (desenhada, ou igual à memória) tem as páginas só de leitura (`track_writes` em `memory/guest_memory.cpp`).
- **Primeira escrita.** A primeira escrita de qualquer outro (PPU, DMA de SPU, HLE) tira a proteção da faixa e a registra. Antes de qualquer `flush`, o renderizador (`apply_cpu_writes`) descarta a cópia dessa superfície e a sobe de novo da memória quando for usada.
- **Escritas vizinhas.** Uma escrita ao lado da faixa, na mesma página, passa por uma instrução com a página liberada (passo único) e a página volta a ser protegida.
- **Escritas do kernel.** O `ReadFile` escreve na memória do convidado pelo kernel, onde a página protegida faria a leitura falhar em vez de gerar exceção. Por isso ele avisa antes (`before_host_write`).
- **Sem `VirtualProtect` página a página.** A proteção muda numa chamada por trecho de páginas contíguas. Página a página, eram ~900 chamadas por superfície de 1280×720, alguns milissegundos por quadro.

Duas condições de corrida apareceram nos testes e estão tratadas:
- reproteger depois do passo único só sob a mesma trava, conferindo a contagem de novo;
- uma falha cuja faixa terminou entre a exceção e o tratamento: a página já está liberada e a escrita é só repetida.

Mais dois acertos para o vídeo:
- **Textura e superfície no mesmo endereço.** O atalho que amostra a própria superfície exige agora textura de cor de 32 bits com o mesmo *pitch*. Antes bastava o endereço, e o plano Y (B8, *pitch* 1280) era lido como a superfície RGBA que ocupara aquela memória: o vídeo saía repetido 4 vezes na horizontal.
- **Pós-processamento nas SPUs.** O menu passa por ele. A cada quadro, o RSX copia a imagem para a memória principal (`0x31E00000` e `0x32200000`), as SPUs a processam e o RSX a copia de volta. Antes, o sly4 ignorava a escrita das SPUs. Com a proteção, ela passou a valer, mas as SPUs liam uma memória que nunca recebera a cópia, e o fundo 3D saía preto. Uma cópia 2D para a memória principal agora é escrita lá na hora, antes de o RSX seguir para o *label* que avisa o jogo.

### Custo

O menu caiu de ~41–47 para ~37–41 quadros/s. Os 41–47 vinham, em parte, de não emular o pós-processamento das SPUs. Agora cada quadro escreve a imagem na memória principal e a sobe de novo depois das SPUs. O que já foi recuperado:

| Medida | Antes | Depois |
|---|---|---|
| `VirtualProtect` página a página | uma chamada por trecho contíguo | ~23 → ~27 quadros/s (com o item abaixo) |
| Reordenação de bytes na escrita e na subida de superfícies, byte a byte (~10 ms por 1280×720) | uma rotação ou `bswap` por palavra | ~27 → ~30 |
| Um único *command buffer*, entregue à GPU só nos pontos de espera | 64 *command buffers*: a cada troca de passada o anterior vai para a GPU sem esperar (`kick`), e a escrita de volta só espera o resto | ~30 → ~37–41 |

O que falta é o tempo da GPU antes de cada escrita de volta (~2,5 ms) e a cópia (~1 ms). Uma escrita de volta assíncrona, até o próximo *label*, ou a conversão de formato na própria GPU, pode recuperar parte disso.

## Ferramentas desta fase

| Opção | O que faz |
|---|---|
| `--fs-reads` | registra cada leitura de arquivo, com posição e tamanho |
| `--watch-write 0xADDR[,tamanho]` | registra cada escrita nesses bytes (*stores* comuns também): thread, LR e o código do host (RVA, para o `nm`) |
| `--dump-mem` | agora grava zeros nas páginas que não existem, em vez de parar no meio |
| `--no-audio`, `--dump-audio arquivo.wav` | ver 6b |

Os acentos em `app/main.cpp` e `runtime/kernel/kernel.cpp` estavam codificados duas vezes ("instruÃ§Ãµes" no resumo) e foram corrigidos.
