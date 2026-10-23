# Primeira fase jogável (Fase 7)

| Entrega | Estado |
|---|---|
| 7a. Novo jogo até a tela de carregamento | **Concluída** (2026-10-23) |
| 7b. O carregamento do Prólogo em Paris terminando | **Concluída** (2026-10-23) |
| 7c. A fase carregada e renderizada | **Concluída** (2026-10-23) |
| 7d. Física (Havok) e IA rodando sem travar | **Concluída** (2026-10-23); 420 s de jogo |
| 7e. Sly andando e pulando com o controle | **Concluída** (2026-10-23) |

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

As semânticas SPU (`runtime/cpu/spu_sem_float.inl`) agora seguem o formato da SPU:
- **Comparações** (`fceq`, `fcgt`, `fcmeq`, `fcmgt`) são feitas nos bits: sinal e magnitude ordenados como inteiros, com zero e denormais iguais a 0. Isso é mais barato que comparar floats e vale para o expoente 255.
- **Aritmética** (`fa`, `fs`, `fm`, `fma`, `fms`, `fnms`, `frest`, `frsqest`) continua nos floats do host. Só as pistas que saem infinitas ou NaN (algum operando com expoente 255, ou um resultado que estourou) são refeitas em double. O expoente 255 vira 2¹²⁸·1,f, e o que passa do maior valor satura em ±`0x7FFFFFFF`, como no hardware.
- **Conversões** `cflts`/`cfltu` leem o expoente 255 como número (saturam pelo sinal).
- **Arredondamento:** continua o do host (ao mais próximo). A SPU trunca, mas isso só muda o último bit.

O RPCS3 faz o mesmo nas comparações, no modo padrão ("approximate xfloat": `fcgt` compara os bits como inteiros quando há expoente 255).

**Resultado:** a SPU da física passou a esperar canais a maior parte do tempo (133 s de 300, contra 10 s antes). O carregamento termina, a cena do Bentley e do Murray roda, e o Sly aparece na rua a ~166 s da execução, depois o aviso do ícone de salvamento. É o mesmo caminho do RPCS3, que leva ~135 s até a cena. O NavGraph da fase (`Levels/Paris/Streets/NavGraph.bfx_res`, 235 KB) é lido do psarc junto com o resto.

Os testes de semântica ganharam os casos do formato estendido: 2¹²⁸·0 = 0, saturação de `fm`/`fa`, ordem de `fcgt`/`fceq`/`fcmgt` com `0x7FFFFFFF`, ±0 e denormais, e `cflts` com expoente 255. O spudiff segue com 713.770 casos iguais entre os blocos compilados e o interpretador.

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

## Ferramentas desta fase

| Opção | O que faz |
|---|---|
| `--dump-spu-ls pasta` | no fim, grava o LS (256 KiB) e os 128 registradores de cada SPU ativa (`spu_<id>.ls`), para desmontar com `spu-elf-objdump -b binary -m spu -D` |
| `--spu-ls-watch 0xLSA` | registra cada bloco SPU, em qualquer SPU, depois do qual aquela palavra do LS mudou, com os registradores da entrada; sem a opção, o laço dos blocos não muda |
| `--press nome@s:duração` | segura o botão; `lup`/`ldown`/`lleft`/`lright` movem o analógico esquerdo; pressões sobrepostas valem juntas |
| `--dump-frames pasta,N,F` | a cada N quadros, a partir do *flip* F |
| `--rsx-capture N,arquivo,K` | K quadros a partir de N, um arquivo cada, com as constantes dos *fragment programs* |

## O que fica para a Fase 8

- As fases e minijogos seguintes, os guardas com o NavGraph em ação e a jogabilidade nas outras épocas.
- O código SPU novo da fase (carregamento e jogo) ainda roda em parte no interpretador. Gravá-lo com `--spu-cache` e compilá-lo deve ajudar no desempenho.
- A comparação quadro a quadro com o RPCS3 dentro da fase.
