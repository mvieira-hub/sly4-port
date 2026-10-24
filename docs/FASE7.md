# Primeira fase jogável (Fase 7)

| Entrega | Estado |
|---|---|
| 7a. Novo jogo até a tela de carregamento | **Concluída** (2026-10-23) |
| 7b. O carregamento do Prólogo em Paris terminando | **Concluída** (2026-10-23) |
| 7c. A fase carregada e renderizada | **Concluída** (2026-10-23) |
| 7d. Física (Havok) e IA rodando sem travar | **Concluída** (2026-10-23); 420 s de jogo |
| 7e. Sly andando e pulando com o controle | **Concluída** (2026-10-23) |
| 7f. Jogando pela janela: 30 quadros/s | **Concluída** (2026-10-24); controle Xbox por Bluetooth |
| 7g. Som limpo na fase | **Concluída** (2026-10-24) |
| 7h. MFC e operações atômicas como no RPCS3 | **Concluída** (2026-10-24) |
| 7i. Sem pontos brancos nas bordas (pós-processamento nas SPUs) | **Concluída** (2026-10-24); `frest`/`fi` como no SPU |

**Saída da fase (alcançada):** a primeira fase carrega, renderiza e o Sly anda e pula com o controle.

Duas correções fizeram isso:
- **SPU:** o ponto flutuante simples segue o formato estendido. O `0x7FFFFFFF` era lido como NaN e travava o carregamento na física (7b).
- **GPU:** o *clear* respeita os canais pedidos. Limpar só o alfa apagava a imagem da cena (7c).

## 7a. Do menu à tela de carregamento

O roteiro usado nos testes (pasta de dados nova, sem save de progresso):

```powershell
.\build\mingw-release\sly4.exe --game PS3_GAME --no-window --render --no-audio --data work\data_t2 --seconds 400 --press "start@40,start@43,cross@50,circle@70" --firmware work\firmware
```

Start no título, Start no menu, Cross em "Novo jogo" e Circle ("não") na pergunta sobre a história completa. O jogo cria o save de progresso pelo `cellSaveDataListSave2` da 6a e mostra a tela de carregamento com dicas e colecionáveis, como o RPCS3.

No RPCS3, com a mesma sequência (`tools\rpcs3_capture.ps1`), a tela de carregamento aparece a ~114 s, e a cena do roubo no museu, a ~135 s. No sly4, a tela ficava parada para sempre (testado até 450 s).

## 7b. O carregamento que não terminava

### Onde o tempo ia

Com o carregamento parado, as threads PPU e os grupos SPU eram os mesmos do RPCS3, e as leituras do psarc continuavam devagar. Cinco SPUs estavam ~90% ociosas, mas a sexta, o SPURS de uma SPU que roda a física (*taskset* `0x337EC280`, tarefa 0), estava ocupada o tempo todo: ~230 bilhões de instruções em 400 s. O resumo do fim mostrava sempre o mesmo laço, em LS `0x3838`:

```
03848 il      $8,20
0384C lqd     $10,16($5)      ; nó->dono = $4
...
03860 lqx     $2,$5,$8        ; $5 = nó->próximo (+20)
03868 ceq     $3,$7,$5
0386C brz     $3,0x3848       ; até voltar ao primeiro nó
```

É o percurso de um anel (lista circular): cada nó recebe o novo dono, até a volta ao nó inicial. Com o LS despejado no fim (`--dump-spu-ls`, novo) e desmontado (`spu-elf-objdump -b binary -m spu`), o anel estava quebrado. O "próximo" de um nó era 0, e o endereço 0 do LS apontava de volta para esse nó. O percurso ficava preso no ciclo `0 ↔ 0x1BD98` e nunca voltava ao início.

### Quem quebrava o anel

`--spu-ls-watch 0x14` (novo) registra cada bloco SPU depois do qual uma palavra do LS mudou, com os registradores da entrada. Só um bloco escreveu ali: o da função em `0x9780`, chamada com dois argumentos nulos (`$4 = $5 = 0`). Ela juntava dois anéis (provavelmente as ilhas de simulação do Havok: corpos que se tocam são resolvidos juntos) e gravou `*(0 + 20)`.

Os nulos vinham de `0x3D88`, uma busca do par de nós mais próximo entre dois anéis. A distância ao quadrado de cada par era comparada (`fcgt`) com a melhor até ali, e a busca começava com o melhor valor `0x7FFFFFFF`, lido de LS `0x1AB90`.

### O formato da SPU

Em precisão simples, a SPU não tem infinito nem NaN. O expoente 255 é só mais uma faixa de números (até ~6,8·10³⁸), e `0x7FFFFFFF` é o maior valor que existe. Para o x86, esse padrão é um NaN, e toda comparação com NaN é falsa. Nenhuma distância era "menor que o melhor", a busca devolvia nulos e o anel se corrompia.

As semânticas SPU (`runtime/cpu/spu_sem_float.inl`) seguem agora as regras do RPCS3 no modo padrão ("approximate xfloat"):
- **Comparações** (`fceq`, `fcgt`, `fcmeq`, `fcmgt`) são feitas nos bits: sinal e magnitude ordenados como inteiros, com zero e denormais iguais a 0. Isso vale para o expoente 255 e é o que conserta a física.
- **Aritmética:** fica a IEEE do host, com uma regra da SPU: zero vezes qualquer coisa é zero (`fm`, `fma`, `fms`, `fnms`), mesmo vezes infinito ou NaN. Todo NaN vira o NaN padrão.
- **Arredondamento:** ao mais próximo, como no RPCS3. A SPU trunca, mas isso só muda o último bit.

Uma primeira versão emulava o formato estendido também na aritmética: o que estourava saturava em ±`0x7FFFFFFF`, refeito em double, e o arredondamento truncava. Ela foi trocada pelas regras do RPCS3, as mais testadas, depois de se ver que ela não mudava os pontos brancos da 7g e que a física só dependia das comparações.

**Resultado:** a SPU da física passou a esperar canais a maior parte do tempo (133 s de 300, contra 10 s antes). O carregamento termina, a cena do Bentley e do Murray roda, e o Sly aparece na rua a ~166 s da execução, depois o aviso do ícone de salvamento. É o mesmo caminho do RPCS3, que leva ~135 s até a cena. O NavGraph da fase (`Levels/Paris/Streets/NavGraph.bfx_res`, 235 KB) é lido do psarc junto com o resto.

Os testes de semântica ganharam os casos novos: zero vezes infinito em `fm` e `fma`, e a ordem de `fcgt`/`fceq`/`fcmgt` com `0x7FFFFFFF`, ±0 e denormais. O spudiff segue com todos os casos iguais entre os blocos compilados e o interpretador (921.395).

## 7c. A cena que apagava

Com a fase rodando, a imagem do mundo ficava preta por trechos de 14 a 41 quadros seguidos, e só o HUD e o cone do holofote apareciam. Para achar a causa vieram duas ferramentas:
- `--dump-frames pasta,1,6000` grava todos os quadros a partir do *flip* 6000 (o terceiro número é novo);
- `--rsx-capture 6000,arquivo.txt,90` grava os comandos de 90 quadros, um arquivo por quadro, agora com as constantes do *fragment program* de cada desenho (elas ficam na memória, não nos métodos).

Na mesma rodada, um quadro bom e um preto diferiam em uma coisa. Antes da cópia da cena para a memória principal (a ida e volta pelas SPUs da 6d), o quadro preto tinha:
1. `CLEAR_SURFACE 0x80` na imagem da cena;
2. um desenho;
3. a cópia de 82×79 pixels dessa imagem para `0x01240000`.

O jogo limpa só o **alfa** da imagem (bits R 0x10, G 0x20, B 0x40, A 0x80), desenha nele quanto do holofote está à vista e lê esse pedaço, provavelmente para o brilho na câmera. O `clear` do renderizador limpava os quatro canais sempre que algum bit de cor vinha ligado, e apagava a cena.

O `vkCmdClearAttachments` não tem máscara de canais. Para um *clear* parcial, `clear_channels` (`gpu/rsx_vk.cpp`) desenha um triângulo que cobre o alvo:
- o *vertex shader* só usa `gl_VertexIndex`;
- o *fragment shader* escreve 1,0;
- o *blend* `CONSTANT_COLOR`/`ZERO` deixa no alvo as *blend constants*, que recebem a cor do *clear*;
- o `colorWriteMask` é o dos bits pedidos.

Os dois *shaders* são escritos direto em SPIR-V pelo mesmo construtor dos tradutores. Antes: 55 quadros pretos em 407. Depois: 0 em 749.

## 7d e 7e. Jogando

O `--press` agora segura botões (`cross@192:0.5`), aceita pressões sobrepostas e move o analógico esquerdo (`lup`, `ldown`, `lleft`, `lright`). Assim dá para jogar por roteiro, sem janela. O gamepad e o teclado da janela usam o mesmo caminho do `cellPad` (6c).

```powershell
.\build\mingw-release\sly4.exe --game PS3_GAME --no-window --render --no-audio --data work\data_t1 --seconds 205 --firmware work\firmware --dump-frames work\frames,3,5950 --press "start@40,start@43,cross@50,circle@70,cross@185,cross@192,cross@195,cross@195.5,lup@198:4,cross@199"
```

Nos quadros, depois do aviso fechado:
- com Cross, o Sly pula até sair do topo da tela e pousa levantando poeira;
- com Cross duas vezes, o pulo duplo sobe mais;
- com o analógico para cima, ele corre pela rua (e pula correndo);
- ao chegar ao terraço, a cena dos helicópteros de segurança roda ("Careful, Sly!"), e o objetivo "Break into the museum" aparece com a dica do pulo duplo.

**Rodada longa:** 420 s, com ~200 s de jogo andando, pulando e virando em vários pontos da fase. Nada travou: física (BFX/Havok nas SPUs), os helicópteros e a câmera seguiram. Foram 12.838 *flips*, com a jogabilidade a ~29 quadros/s sem janela, perto dos 30 do jogo.

## 7f. Jogando pela janela: quadros/s

Jogando com o controle Xbox (Bluetooth) numa janela, num monitor de 144 Hz, a fase rodava a ~19 quadros/s, contra ~29 sem janela. O tempo do RSX nos *flips* era de 157 s em 778 s (~10 ms por quadro), contra 8,7 s em 420 s sem janela. As causas:
- a janela apresentava a imagem a cada *refresh* (144 por segundo), em modo FIFO;
- cada apresentação punha na fila da GPU, compartilhada com o renderizador, uma cópia que esperava o monitor liberar uma imagem (semáforo de `vkAcquireNextImageKHR`), e o trabalho do jogo esperava atrás dela;
- o `vkQueuePresentKHR` bloqueava segurando a trava da fila.

As correções (`gpu/vulkan_context.cpp`, `app/main.cpp`):
- a imagem é esperada na CPU, num *fence* da thread da janela, e não na fila;
- com vsync, o modo é MAILBOX (sem *tearing*, sem bloqueio), com FIFO como reserva;
- a janela só apresenta quando o jogo mostrou um quadro novo; no resto do tempo dorme 1 ms.

**Resultado:** 30–34 quadros/s na fase, com janela; o tempo dos *flips* caiu para 4,4 s em 300 s. O título da janela mostra os quadros/s do jogo (meio segundo) e o log, a média a cada 10 s.

## 7g. Som limpo na fase

Na fase o som engasgava e estalava. Eram três causas:
- **Fila do dispositivo sem reserva.** Os blocos de 5,3 ms iam para o Windows assim que prontos, e o WASAPI puxa de ~10 em ~10 ms. Quando um pedaço chegava com a fila quase vazia, era completado com silêncio, e isso gerava o estalo. Agora a fila mantém ~40 ms, refeitos quando ela seca, e o resumo conta as vezes em que secou.
- **Código SPU da fase sem compilar.** O mixer do jogo (SCREAM, uma tarefa SPURS) atrasava, e a cada ~0,2 s pulava meio anel da porta (8 blocos, 42 ms). Gravar a fase com `--spu-cache` revelou 13.722 blocos SPU novos rodando no interpretador; compilados, são 60.382 blocos ao todo. Os cortes no meio do som caíram de 95 para ~5 em 80 s de fase.
- **Blocos atrasados.** Quando um bloco de uma porta que estava tocando chega vazio, o `cellAudio` espera até 8 períodos (43 ms) sem avançar o índice, enquanto o dispositivo toca da reserva, e recupera o atraso depois (dois blocos por período).

`docker\build.ps1 -Jobs N` limita os compiladores em paralelo: com 152 arquivos SPU gerados, compilar todos de uma vez estourava a memória do contêiner.

## 7h. MFC e operações atômicas, como no RPCS3

Na caça aos pontos brancos, o MFC foi comparado com o `SPUThread.cpp` do RPCS3. Três diferenças foram corrigidas (`cpu/spu_channels.cpp`, `cpu/guest_atomic.cpp`, `kernel/spu_thread.cpp`):
- **Listas de DMA:** cada elemento cai no LS no próximo espaço de 16 bytes, com o mesmo deslocamento que o seu endereço tem (`lsa | (ea & 0xF)`), e não no início do espaço.
- **Reservas:** o `putllc` falha se qualquer escritor tocou a linha desde o `getllar`, mesmo devolvendo os mesmos bytes (o ABA de uma trava pega e solta), e também se um DMA comum gravou nela.
- **DMA atômico por linha:** cada linha de 128 bytes de um DMA passa pela mesma trava das operações atômicas, sem ser rasgada no meio de um `putllc` de outra SPU.

## 7i. Os pontos brancos nas bordas

Na cena de abertura e na fase apareciam pixels brancos isolados na borda dos personagens, uns vinte por quadro, que o RPCS3 não mostra. Vêm do antisserrilhado que o jogo faz nas SPUs (uma tarefa SPURS com o código do EBOOT em `0x00E4C300`, no LS em `0x3000`), e a causa era uma instrução de ponto flutuante exata demais.

### Como o filtro trabalha

É um MLAA em quatro etapas por quadro, com barreira entre elas e dois buffers alternados (`0x31E00000` e `0x32200000`, passo de linha `0x1400`, 768 linhas):
1. **Transposição** (`0x4740`, modo 1): cada ladrilho de 128×128 passa a ser guardado coluna a coluna. Cada trecho de 512 bytes de uma linha do buffer vira um segmento vertical de 128 pixels.
2. **Passagem vertical** (`0x5FB0`, modo 1, `$92 = 0xB40`): o *slot* de 768 pixels do LS é uma coluna inteira (720 úteis + 48 de enchimento), buscada em 6 trechos de 512 bytes a 128 linhas de distância. Os 48 do fim são as linhas 720–767, que não aparecem.
3. **Destransposição** (`0x4740`, modo 2).
4. **Passagem horizontal** (`0x5FB0`, modo 0, `$92 = 0x1400`): faixas de 144 linhas inteiras.

Cada passagem copia a entrada para a saída (`0x7410`), marca as bordas no alfa (`0x6B90`), monta listas de trechos de borda e mistura os pixels de cada trecho (`0x6640`).

### A causa

Para um trecho de borda de `L+1` pixels e cruzamento `c`, a mistura calcula o peso em 16.16 como `w = 2(1−c)/(L+1)`, com `1/(L+1)` vindo de `frest` + `fi`. Depois reduz a 8 bits (`w >> 8`) e aplica `Δ × (w8 + 1)` sobre `cor × 256`, com dois canais por via de 32 bits. O projeto assume `w8 ≤ 255`. Com `L = 0` e `c = 0,5`, `w` dá exatamente 1,0: no sly4, `frest(1,0)` devolvia 1,0 exato, `cfltu` dava `0x10000`, `w8 = 256`, o fator virava 257 e, quando o vizinho é preto, a soma empacotada ficava negativa: bytes `0xFF`, um pixel branco colado na silhueta.

No SPU, `frest`/`frsqest` entregam uma estimativa por interpolação linear (base de 13 bits e passo de 10 bits na mantissa, 16 intervalos), e `fi` completa a interpolação com os 19 bits baixos do operando. A base não representa 2,0, então a estimativa fica um pouco abaixo do valor exato mesmo para potências de dois: `1/1,0` sai `1 − 2⁻¹⁴`, o peso fica `0xFFFC` e `w8 = 255`. É disso que o jogo depende.

**Correção:** `frest`, `frsqest` e `fi` seguem esse formato (`cpu/spu_sem_float.inl`), com teste em `tests/test_spu_semantics.cpp`. Na cena de abertura, 121 quadros capturados (`--dump-frames`, *flips* 3700–4900) não têm mais pixel branco isolado nas bordas (antes, 9–25 por quadro). O diferencial blocos×interpretador segue com todos os casos iguais (3.685.525) e o oráculo do inflate, com os 2.000 blocos iguais.

### Como foi achado

O que foi descartado antes, em ordem: a "cauda" do *slot* (é enchimento invisível, pelo layout ladrilhado); bug de geração de código (os pontos persistem com a tarefa inteira no interpretador); sincronização RSX→memória (a imagem que a SPU vê antes da passagem não tem brancos); corrida entre etapas (linha do tempo dos DMAs das cinco SPUs: 0 violações da ordem T1→V→T2→H); interrupções (nenhuma); LS sobrescrito entre o `get` e o uso (histórico de uma palavra vigiada: sempre get → alfa → cópia); listas de DMA (todas de 512 bytes alinhados).

O que decidiu: uma armadilha nos *stores* do interpretador, disparada quando uma palavra escura do *slot* virava branca, com os 128 registradores. Ela apontou o laço de mistura, cujos termos `Δ × (w8+1)` negativos levaram ao peso 1,0 e ao `frest`.

## Ferramentas desta fase

| Opção | O que faz |
|---|---|
| `--dump-spu-ls pasta` | no fim, grava o LS (256 KiB) e os 128 registradores de cada SPU ativa (`spu_<id>.ls`), para desmontar com `spu-elf-objdump -b binary -m spu -D` |
| `--spu-ls-watch 0xLSA` | registra cada bloco SPU, em qualquer SPU, depois do qual aquela palavra do LS mudou, com os registradores da entrada; sem a opção, o laço dos blocos não muda |
| `--press nome@s:duração` | segura o botão; `lup`/`ldown`/`lleft`/`lright` movem o analógico esquerdo; pressões sobrepostas valem juntas |
| `--dump-frames pasta,N,F` | a cada N quadros, a partir do *flip* F |
| `--rsx-capture N,arquivo,K` | K quadros a partir de N, um arquivo cada, com as constantes dos *fragment programs* e, em PNG, as imagens grandes copiadas para a memória principal e de volta (o que vai para as SPUs e o que volta delas) |

## O que fica para a Fase 8

- As fases e minijogos seguintes, os guardas com o NavGraph em ação e a jogabilidade nas outras épocas.
- O código SPU de cada fase nova precisa ser gravado com `--spu-cache` e compilado (7g).
- A comparação quadro a quadro com o RPCS3 dentro da fase.
