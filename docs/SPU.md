# SPU e SPURS (Fase 4)

O Cell tem, além do PPU, seis SPUs disponíveis para o jogo. Cada um tem um processador próprio, 256 KiB de memória local (*local store*) e acesso à memória principal só por DMA (o MFC). O jogo os usa por meio do SPURS, o sistema de tarefas e *jobs* da Sony, para:
- descompressão (EDGE zlib);
- geometria (EDGE);
- áudio (SCREAM e mp3);
- física (Havok);
- vídeo (Bink).

Esta fase faz o código SPU rodar no PC do mesmo jeito que a Fase 2 fez com o PPU: decodificar, interpretar como referência, gerar C++ e validar um contra o outro. Por cima disso vem o SPURS, que não é reimplementado: o módulo do firmware que o contém (`libsre`) é recompilado e roda como está, como faz o RPCS3 no modo LLE.

| Entrega | Estado |
|---|---|
| 4a. Decodificador SPU validado contra o `spu-elf-objdump` | **Concluída** (2026-10-22) |
| 4b. Interpretador SPU: registradores de 128 bits, local store, canais e DMA | **Concluída** (2026-10-22) |
| 4c. Módulos do firmware: a `libsre.prx` (SPURS, `cellSync`) carregada, recompilada e ligada às importações do jogo | **Concluída** (2026-10-22) |
| 4d. Grupos de threads SPU do LV2 rodando o kernel SPURS real no interpretador; descompressão idêntica à nativa | **Concluída** (2026-10-22) |
| 4e. Gerador de C++ para SPU, por blocos verificados, e teste diferencial contra o interpretador | **Concluída** (2026-10-22) |
| 4f. Saída da fase: conteúdo descomprimido idêntico ao das ferramentas Python e threads de áudio e física rodando sem travar | **Concluída** (2026-10-22) |

## Programas SPU do jogo

O `tools/spu_extract.py` acha 10 ELFs SPU embutidos no EBOOT, com 266.842 palavras de código e dados. Também há *jobs* soltos no `.rodata` (por exemplo o descompressor EDGE em `0xD5F4B0`), que o SPURS carrega quando o jogo os pede.

| Arquivo (offset no EBOOT) | Tamanho | Identificação |
|---|---|---|
| `spu_00E32D00.elf` | 38.100 bytes | Tarefa EDGE zlib inflate (`edgezlib_inflate_task.cpp`), que o FIOS usa para o psarc. Com o firmware (4c), roda no SPU de verdade; sem ele, como código nativo (KERNEL.md, 3d) |
| outros 9 | 34 KB a 174 KB | A identificar |

## 4a. Decodificador SPU

Código em `recomp/spu/`:
- `spu_ops.inc` é a tabela: 199 instruções, cada uma com formato, opcode de 11 bits e operandos na ordem do montador;
- `spu_decoder.cpp` faz a decodificação e o texto.

O SPU tem opcodes de comprimento variável: RRR com 4 bits, RI18 com 7, RI10 com 8, RI16 com 9, RI8 com 10, e RR/RI7 com 11. A decodificação segue a mesma busca do binutils: testa os prefixos do mais curto ao mais longo, e cada um só vale se a instrução encontrada tiver aquele formato.

O texto reproduz o do `spu-elf-objdump`:
- **Sufixos:** `p`/`c`, `d` e `e` nos desvios indiretos, no `sync` e no `hbr` (`bid`, `bisledpe`, `hbrp`).
- **Alvos:** endereços dos desvios relativos limitados a 18 bits.
- **Comentário:** `# hex` quando o último imediato passa de 16.

A tabela de opcodes e o formato vêm do binutils 2.35, o último com SPU (`opcode/spu-insns.h` e `opcodes/spu-dis.c`), consultados numa cópia do gdb 8.3.1.

### Validação

```powershell
.\tools\run.ps1 spu_extract.py --out /work/spu
docker run --rm -v "${PWD}\work:/work" -v "${PWD}\tools:/t:ro" sly4-spu:1 sh /t/spu/objdump_all.sh
# para cada ELF:
.\build\mingw-release\sly4_recomp.exe spu-disasm --elf work\spu\spu_00E32D00.elf --out work\spu\ours\spu_00E32D00.txt
.\tools\run.ps1 spu/compare_disasm.py /work/spu/ref /work/spu/ours
```

O `objdump_all.sh` desmonta, com o binutils 2.35 da imagem `sly4-spu:1`, cada segmento executável dos 10 programas no endereço do local store.

**Resultado: 266.842 palavras, todas iguais ao `objdump`.** Isso inclui os dados no meio do código: as 1.381 palavras que não são instruções saem como `.long` dos dois lados. Não houve nenhuma diferença de texto e nenhuma palavra rejeitada só de um lado.

O `sly4_tests.exe` (`test_spu.cpp`) confere uma amostra dessas palavras e que cada instrução da tabela é alcançável pela decodificação. Isso roda também na CI, que não tem o jogo.

## 4b. Interpretador SPU

Código em `runtime/cpu/`, na biblioteca `sly4_cpu`, junto com o PPU:

| Arquivo | Conteúdo |
|---|---|
| `spu_context.h` | 128 registradores de 128 bits, PC, SRR0, FPSCR, ponteiro para o local store de 256 KiB e o estado do MFC (parâmetros do comando, máscara de tag, reserva de linha de 128 bytes, eventos, decrementador) |
| `spu_semantics.h` + `spu_sem_int.inl`, `spu_sem_float.inl`, `spu_sem_control.inl` | Uma função `inline` por instrução (`spusem::op<Op::X>`), para o interpretador e para o gerador de C++ da 4e |
| `spu_channels.cpp` | Canais: o MFC, decrementador, eventos e SRR0. O resto (mailboxes, sinais, fontes de evento) vai para uma interface `SpuIo` que o SPURS e os grupos de threads SPU implementam na 4d |
| `spu_interpreter.cpp` | Laço: busca no local store, decodifica e executa |

Os registradores usam o mesmo layout do VMX do PPU (bytes invertidos), então a palavra 0 do SPU, o *preferred slot* onde ficam escalares e endereços, é a *lane* 3 do host.

**DMA:**
- `get`/`put` (e as variantes de barreira e *fence*) e as listas (`getl`/`putl`) terminam na hora, e a leitura do status de tag devolve a máscara inteira;
- os atômicos de 128 bytes (`getllar`, `putllc`, `putlluc`) usam uma reserva com comparação dos 128 bytes, protegidos por um lock global.

**Ponto flutuante simples:** o SPU não segue a IEEE: não tem denormais, infinitos nem NaN, e trunca. O expoente 255 é só mais uma faixa de números, e `0x7FFFFFFF` (~6,8·10³⁸) é o maior valor. As semânticas seguem as regras do RPCS3 no modo padrão ("approximate xfloat"; ver FASE7.md, 7b):
- denormais zerados na entrada e na saída;
- comparações feitas nos bits (sinal e magnitude como inteiros), o que vale para o expoente 255;
- zero vezes qualquer coisa é zero nas multiplicações;
- o resto é a IEEE do host, com todo NaN feito o NaN padrão (`0x7FC00000`; no duplo, `0x7FF8000000000000`). Qual NaN de entrada uma FMA do x86 repassa depende da ordem de operandos que o compilador escolhe, e sem isso o interpretador e o código gerado divergiam (4e).

O arredondamento é o do host (ao mais próximo). `frest`/`frsqest` entregam a estimativa no formato do SPU (base de 13 bits e passo de 10 bits na mantissa, por intervalo da mantissa do operando) e `fi` completa a interpolação; a estimativa fica um pouco abaixo do exato mesmo para potências de dois (`1/1,0 = 1 − 2⁻¹⁴`), e o antisserrilhado do jogo depende disso (FASE7.md, 7i). O ponto flutuante duplo é IEEE, como no SPU.

### Oráculo: o próprio inflate do jogo

```powershell
.\build\mingw-release\sly4_recomp.exe spu-oracle --spu-elf work\spu\spu_00E32D00.elf --psarc PS3_GAME\USRDIR\Sly4.psarc --blocks 2000 --seed 7
```

A tarefa EDGE zlib tem uma função interna (`0x3740`) que faz o serviço completo: busca os dados comprimidos por DMA, descomprime no local store e devolve por DMA. O próprio laço da tarefa mostra os argumentos dela (`0x3248`–`0x3274`).

O comando faz o seguinte:
1. sorteia blocos comprimidos do `Sly4.psarc`;
2. carrega a tarefa num local store novo e põe o bloco na memória principal;
3. chama a função no interpretador, com `$1` montado como manda o ABI do SPU (palavra 1 com o espaço livre de pilha, que os prólogos conferem com `heqi`);
4. compara a saída com o inflate nativo (`runtime/core/inflate.cpp`).

**Resultado: 2.000 blocos sorteados do psarc inteiro, todos idênticos byte a byte.** Foram 4,38 bilhões de instruções SPU interpretadas em 23,7 s (cerca de 185 milhões por segundo). Isso cobre 80 instruções diferentes: inteiras, lógicas, deslocamentos e rotações de quadword, `shufb`/`selb`, comparações, desvios, *hints*, local store, canais e DMA.

### Testes do que o oráculo não alcança

O `sly4_tests.exe` (`test_spu_semantics.cpp`) confere, com valores calculados a partir da ISA:
- as multiplicações 16×16;
- `clz`/`cntb`, carry e borrow estendidos e `sumb`/`avgb`/`absdb`;
- `fsmb`/`fsmh`/`gb`/`orx`, rotações e deslocamentos por elemento e as máscaras de inserção;
- as constantes especiais do `shufb`;
- ponto flutuante simples: o `fma` e suas variantes, comparações, as conversões com escala e saturação, e denormal tratado como zero;
- ponto flutuante duplo: soma, `fma`, `fesd`/`frds` e `dftsv`.

## 4c. Módulos do firmware (LLE)

O plano original era reimplementar o `cellSpurs` em HLE. Isso foi trocado pelo caminho do RPCS3 no modo LLE: o SPURS do PPU e o kernel SPURS que roda nos SPUs estão no módulo `libsre.sprx` do firmware, que é carregado e recompilado como o EBOOT. Os motivos:
- o SPURS tem 149 funções exportadas, *workloads*, *tasksets*, *jobchains*, prioridades e eventos, com muito comportamento não documentado;
- o jogo usa partes que o 3d não cobre (o motor Ape, SCREAM e Havok montam seus próprios *tasksets*);
- com o módulo real, o "o que o SPURS faz" deixa de ser uma dúvida: falta só implementar bem o LV2 que ele usa (threads SPU, eventos, memória).

O módulo não é versionado. Ele vem do firmware instalado no RPCS3 (o do disco, `PS3UPDAT.PUP`) e é descriptografado pelo próprio RPCS3, como o EBOOT:

```powershell
$r = "C:\caminho\do\rpcs3"
Copy-Item "$r\dev_flash\sys\external\libsre.sprx" work\firmware\
& "$r\rpcs3.exe" --decrypt "$((Resolve-Path work\firmware\libsre.sprx).Path)"   # gera libsre.prx
.\build\mingw-release\sly4_recomp.exe emit --prx work\firmware\libsre.prx --base 0x0F000000 --name libsre --out-dir generated\fw_libsre
.\docker\build.ps1
.\build\mingw-release\sly4.exe --game PS3_GAME --no-window --seconds 25 --firmware work\firmware
```

**Carregador de PRX** (`runtime/loader/prx_loader.cpp`): o `libsre.prx` é um ELF64 do tipo `0xFFA4` (PRX da Sony) com relocações próprias (segmento `0x700000A4`, entradas de 24 bytes). O carregador:
- põe o módulo num endereço fixo da área de firmware (`0x0F000000`–`0x0FFFFFFF`, a mesma do RPCS3), para o código gerado valer em toda execução;
- aplica as relocações (tipos 1, 4, 5, 6, 10, 11, 38, 44 e 57);
- lê o *module info*, as exportações (6 bibliotecas: `cellSpurs` com 149 funções, `cellSync` com 42, `cellSheap`, `cellOvis`, `cellDaisy` e `cellSpudll`) e as importações (30 do `sysPrxForUser` e 2 do `cellLibprof`);
- acha as funções de entrada pelas relocações de 32 bits que apontam para código seguido do TOC (os descritores de função), já que um PRX não tem tabela de símbolos.

**Recompilação:** o `sly4_recomp emit --prx` gera 384 funções em `generated/fw_libsre`. O CMake cria uma biblioteca para cada `generated/fw_*` e um `gen_firmware.cpp` que registra todas; sem módulos (como na CI), essa função fica vazia. As tabelas de funções do jogo e dos módulos se juntam numa só, ordenada.

**Ligação:** o HLE agora liga cada importação em duas etapas. Se algum módulo carregado exporta aquele NID, o slot recebe o descritor da exportação e a chamada vai direto ao código recompilado do módulo. Se não, continua o *thunk* HLE. No jogo, 64 importações passam a ir para a `libsre` e 219 continuam em HLE. O kernel chama o `module_start` de cada módulo antes do ponto de entrada do jogo.

**O que a `libsre` pediu do resto:** as funções de C do `sysPrxForUser` que antes só devolviam `CELL_OK` (`_sys_memset`, `_sys_memcpy`, `_sys_strcpy`, `_sys_strcat`, `_sys_printf`, `_sys_snprintf` e parentes) ganharam implementação de verdade em `hle_libc.cpp`, com um formatador de `printf` que lê os argumentos variáveis do ABI do PPC64. Sem isso o SPURS montava nomes e tabelas vazios, e o FIOS falhava com "couldn't start decompression job".

## 4d. Threads SPU do LV2

Código em `runtime/kernel/spu_thread.cpp` e `syscalls_spu.cpp`. Cada thread SPU roda numa thread do Windows com o interpretador da 4b, e fala com o PPU como no LV2 (e como no `sys_spu.cpp` do RPCS3):

| Mecanismo | Implementação |
|---|---|
| Imagens | `sys_spu_image_import`/`open` monta a tabela de segmentos a partir do ELF SPU; o `group_start` copia os segmentos para o local store |
| Grupos | criar, destruir, iniciar, `join`, `terminate`, prioridade e a fila de evento `RUN` |
| Mailboxes | entrada de 4 posições e saída de 1, que o PPU alcança pelas syscalls |
| Sinais | `SNR1`/`SNR2` com modo sobrescrever ou OR |
| `SPU_WrOutIntrMbox` | código < 64: `sys_spu_thread_send_event` (o resultado volta pela mailbox de entrada); < 128: `throw_event`; 128 e 192: `sys_event_flag_set_bit` |
| *Stop* | `0x100` *yield*, `0x101` fim do grupo, `0x102` fim da thread, `0x110`/`0x111` `receive_event` numa fila ligada |
| Eventos | `RdEventStat` com mailbox, sinais e perda de reserva da linha (`LlrLost`), detectada comparando os 128 bytes reservados; o kernel SPURS dorme esperando esse evento |

São as syscalls 156, 158, 169–173, 177–182, 184–188, 190–194, 251, 252 e 260, mais o `sys_spu_image_import` do `sysPrxForUser`. Uma leitura de canal bloqueante desiste depois de ~1 ms e é repetida; assim o laço da thread vê pedidos de parada e conta as instruções.

**Resultado.** Com a `libsre`, o jogo cria dois grupos do SPURS: o `ApeCoreSpurs`, com 5 SPUs, e o `ApeSpurs_1SPU`, com 1. Os seis SPUs rodam o kernel SPURS real (de 8 a 20 milhões de instruções cada até o `cellGcmInit`) e ficam ociosos esperando trabalho. O FIOS manda a primeira descompressão do psarc por uma LFQueue; o SPURS carrega a tarefa EDGE zlib num SPU e ela descomprime o bloco. O jogo segue até o `cellGcmInit`, como na Fase 3.

**Verificação contra o atalho nativo.** O `--dump-mem endereço,tamanho,arquivo` grava a memória do convidado quando o jogo para. Rodando com e sem `--firmware` e comparando 1 MiB em volta do destino:
- os 32 KiB entregues pela tarefa (bloco `0x30AC5A02`, 52.894 → 65.536 bytes, janela de 32.768 bytes em `0x30A09AA0`) são idênticos;
- no MiB inteiro só diferem 3 bytes, um contador de tempo em `0x30A09A24`.

O `sly4_tests.exe` (`test_spu_thread.cpp`) roda um programa SPU que lê a mailbox de entrada, devolve o valor pela de saída e sai com `stop 0x102`, e confere a perda de reserva da linha e a junção das tabelas de funções.

## 4e. Gerador de C++ para SPU

No PPU o código é fixo: o EBOOT inteiro vira C++ função por função. No SPU não. O kernel SPURS, as tarefas e os *jobs* chegam ao local store por DMA durante o jogo, muitas vezes no mesmo endereço, e o mesmo endereço pode ter códigos diferentes em momentos diferentes. Por isso o gerador trabalha por **blocos verificados**, como o RPCS3:
- um bloco é a sequência de palavras a partir de um endereço de entrada até a primeira depois da qual a execução nunca segue (desvio incondicional, `stop` ou palavra que não é instrução); os desvios condicionais no meio só saem do bloco quando tomados;
- cada bloco compilado guarda os bytes do código de que veio, e só roda quando o local store tem exatamente esses bytes naquele endereço (`memcmp` na entrada). Se não houver bloco que confira, o interpretador roda até o próximo desvio.

### De onde vêm os blocos

| Fonte | Como |
|---|---|
| Cache SPU | `sly4.exe --spu-cache work\spu_cache.bin` grava cada bloco que o interpretador executa (endereço e bytes). Cobre o que só existe em tempo de execução: o kernel SPURS e o módulo de *taskset* da `libsre`, tarefas e *jobs*. Execuções seguidas acumulam no mesmo arquivo |
| ELFs SPU | os 10 programas do EBOOT, analisados de forma estática: a entrada, os alvos dos desvios diretos, os pontos de retorno das chamadas e a palavra depois de cada desvio incondicional |

```powershell
.\build\mingw-release\sly4.exe --game PS3_GAME --no-window --seconds 30 --firmware work\firmware --spu-cache work\spu_cache.bin
$elfs = Get-ChildItem work\spu\spu_*.elf | ForEach-Object { "--elf"; $_.FullName }
.\build\mingw-release\sly4_recomp.exe spu-emit --cache work\spu_cache.bin @elfs --out-dir generated\spu
.\docker\build.ps1
```

### O código gerado

Cada bloco vira uma função que chama as mesmas semânticas do interpretador (`spusem::op<Op::X>`), com a palavra da instrução constante, de modo que o compilador resolve os campos e os registradores na compilação. Só as instruções que podem sair do bloco (desvios, `stop`, *halts*, canais que esperam) testam o resultado; as que leem o PC (`lqr`, `stqr` e os desvios relativos) recebem o endereço antes. A contagem de instruções é a mesma do interpretador.

Com a cache de uma execução (496 blocos) mais os 10 ELFs: **22.370 blocos, 519 mil instruções, 1,4 milhão de linhas** em 57 arquivos, que compilam em pouco mais de 1 minuto com `-O2`. O CMake monta a biblioteca `sly4_gen_spu` quando `generated/spu` existe (como os outros, não é versionado); sem ela, tudo roda no interpretador. `--spu-interp` força o interpretador mesmo com os blocos compilados.

### Validação

O `sly4_spudiff.exe` (construído junto com os blocos) faz duas coisas:

```powershell
.\build\mingw-release\sly4_spudiff.exe --cases 64 --seed 7
.\build\mingw-release\sly4_spudiff.exe --oracle --spu-elf work\spu\spu_00E32D00.elf --psarc PS3_GAME\USRDIR\Sly4.psarc --blocks 2000 --seed 7
```

1. **Diferencial por bloco.** Cada bloco roda de estados aleatórios (os 128 registradores e o local store inteiro sorteados, com o código do bloco no lugar; metade dos casos com valores pequenos, para os desvios irem para os dois lados), uma vez no interpretador e uma vez compilado. São comparados os registradores, o local store, o PC, o estado do MFC e dos eventos, e a contagem de instruções. Ficam de fora os blocos que disparam DMA (precisam de memória principal) ou usam o decrementador (depende do relógio), 1.078 de 22.370, e os casos em que uma escrita aleatória reescreve o próprio bloco.
   - A primeira rodada achou 2.047 casos diferentes, todos no sinal de um NaN em `fma`/`fm`. A correção foi tornar os NaN canônicos (ver a nota de ponto flutuante na 4b).
   - **Resultado: 1.362.356 casos (64 por bloco), todos iguais.**
2. **Oráculo com o código compilado.** O mesmo teste da 4b, com a função de inflate da tarefa EDGE zlib rodando pelos blocos compilados: **2.000 blocos do psarc idênticos ao inflate nativo**, com exatamente as mesmas 4.381.465.286 instruções do interpretador, em 7,3 s em vez de 24,1 s (3,3 vezes mais rápido, cerca de 600 milhões de instruções SPU por segundo).

No jogo, com a `libsre` e os blocos compilados, só 1 bloco novo passou pelo interpretador até o `cellGcmInit`, e o bloco descomprimido pela tarefa EDGE zlib sai idêntico ao do atalho nativo.

**Até o menu (Fase 5).** O vídeo do Bink, a tela de título e o menu trazem muito código SPU novo, como os *jobs* de geometria EDGE do 3D. Foram 15.430 blocos que rodavam no interpretador. Eles foram gravados com `--spu-cache` numa execução que passa pelo menu (`--press start@45,start@47`) e compilados:
- **45.674 blocos, 2,75 milhões de instruções**, em 116 arquivos (5 minutos de compilação);
- o diferencial deu **700.328 casos, todos iguais** (16 por bloco; 1.852 blocos deixados de lado por DMA ou decrementador).

## 4f. Saída da fase

O critério pede o conteúdo descomprimido igual ao das ferramentas Python e as threads de áudio e física rodando. Para o jogo chegar lá foi preciso:
- um `cellGcm` mínimo, um `cellAudio` e um `cellPad` (início da Fase 5; ver [GPU.md](GPU.md), 5a);
- três correções no lado SPU, que só apareceram com o motor de áudio do jogo.

### O que o motor de áudio (BRB/SCREAM) exigiu

O SCREAM mixa o som num *workload* SPURS próprio, no SPURS de 1 SPU. O módulo de política dele é código SPU cru dentro do EBOOT (`0xF02100`), sem cabeçalho ELF. Até ele funcionar, o jogo esperava 10 s e desistia com `brb_StartSession(): s_pPrimaryOutput 10 sec wait TIME-OUT!`.

1. **Interrupções do SPU.**
   - O mixer grava um `bra 0xA2C` no endereço 0 (o vetor de interrupção) e, no laço principal, abre uma janela com `bie`/`bid`.
   - Agora os desvios indiretos tomados e o `iret` aplicam os flags E (habilita) e D (desabilita).
   - Com interrupções ligadas e um evento da máscara pendente, o SPU salta para 0 com o PC em SRR0 e as interrupções desligadas. Isso é verificado entre blocos e a cada desvio tomado.
   - O decrementador passou a gerar o seu evento (0x20) quando fica negativo.
2. **Listas de DMA com *stall-and-notify*.**
   - O tratador de interrupção do mixer atende o evento de parada de lista (0x2).
   - Um elemento com o bit 31 ligado agora para a lista depois da sua transferência, marca a *tag* em `MFC_RdListStallStat` e gera o evento.
   - A lista continua quando o SPU confirma no `MFC_WrListStallAck`.
3. **Atomicidade entre PPU e SPU.**
   - O `stwcx.`/`stdcx.` do PPU é um CAS de 4 ou 8 bytes; o `putllc` do SPU compara e copia 128 bytes.
   - Uma atualização do PPU que caísse entre a comparação e a cópia era apagada. Assim um `cellSpursRunJobChain` podia se perder e o jogo esperar para sempre, de forma intermitente.
   - Agora os dois tomam a mesma trava por linha de 128 bytes (`cpu/guest_atomic.h`).
   - Cada escrita atômica numa linha também acorda os SPUs que esperam a perda de reserva, em vez de eles descobrirem por *polling*.
4. **Escritas comuns na linha de um `putllc`** (a trava do boot nas tarefas do Havok, Fase 5).
   - Sintoma: em 1 a cada 3 boots, mais ou menos, o jogo parava para sempre. Ou os 5 SPUs giravam nos semáforos do Havok enquanto uma tarefa pronta ficava sem SPU, ou todas as tarefas saíam e o `main_thread` ficava esperando uma delas.
   - Causa: a fila de *jobs* do Havok (`hkJobQueue`, em `0x33D76E20`) guarda na mesma linha de 128 bytes:
     - o `cellSyncMutex` que as tarefas SPU disputam com `getllar`/`putllc`;
     - o dono e a recursão da seção crítica do PPU, e um `sys_lwmutex`, que o PPU grava com *stores* comuns.
   - O `putllc` comparava a linha com a reserva e copiava os 128 bytes do local store. Um *store* do PPU que caísse entre a comparação e a cópia era apagado pelo valor antigo. O `--watch-line` mostrou isso umas 14 vezes por execução.
   - Com o dono da seção crítica errado, o PPU entrava na fila sem trava, ao mesmo tempo que um SPU. Assim se perdia, por exemplo, a contagem de tarefas esperando trabalho.
   - Agora o `putllc` grava só os bytes que o SPU mudou, palavra a palavra com *compare-and-swap* (`guest_store_line_changes`). O `stwcx.` passou a ser um CAS de verdade sobre a palavra.

No caminho também foram corrigidos:
- **Esperas curtas.** Com o MinGW, `sleep_for` abaixo de 1 ms não dorme. O `sys_timer_usleep`, o RSX, o relógio do áudio e as esperas dos SPUs agora usam um timer de alta resolução do Windows (`core/host_sleep.h`).
- **Justiça do mutex do LV2.** O unlock com threads esperando entrega a posse à mais antiga, como no LV2. Sem isso a thread de som do Bink, que faz `trylock`/`unlock` em laço, matava de fome a thread de render.

### Resultado

**Descompressão.** `sly4.exe ... --firmware work\firmware --verify-zlib` calcula, com o inflate nativo, cada pedido que o jogo manda à tarefa EDGE zlib. Quando a thread que pediu volta da espera, compara com o que o SPU escreveu.
- Em 120 s de boot: **2.577 blocos (77,4 MiB) descomprimidos pela tarefa SPU real, todos idênticos**.
- O elo com as ferramentas Python fecha no arquivo inteiro. `sly4_recomp psarc-hash` (inflate nativo) e `tools/psarc_blockhash.py` (`zlib` do Python) produzem o CRC32 de cada bloco descomprimido, e as listas são idênticas: **148.603 blocos, 9,01 GiB**.

```powershell
.\build\mingw-release\sly4_recomp.exe psarc-hash --psarc PS3_GAME\USRDIR\Sly4.psarc --out work\psarc_blocks_cpp.txt
.\tools\run.ps1 psarc_blockhash.py --out /work/psarc_blocks_py.txt
```

**Threads.** Em 120 s o jogo sobe até o vídeo de abertura (`Movies/SanzaruSplash.bik`), com estas threads ativas e sem travar:

| Área | Threads | Estado |
|---|---|---|
| Áudio | `brbDriver`, `brbClient`, 2 × `snd_stream_parsing_thread`, `Bink Snd` | o SCREAM inicia (`brb_StartSession(): OK`, como no RPCS3), o mixer SPU roda por interrupções e o `cellAudio` consome as duas portas no ritmo de 48 kHz |
| Física | `hkSpuUtil Helper` (Havok), `BFX async sim thread` | criadas e esperando trabalho, que só vem com uma fase carregada |
| SPU | 6 SPUs em 2 grupos SPURS | kernel SPURS, tarefas, *jobchains* e o mixer de áudio, quase tudo em código compilado (só 74 blocos novos no interpretador em 2 minutos) |

O vídeo do Bink ainda não avança, e o áudio sai em silêncio: o próximo trabalho é o do vídeo e da GPU (Fase 5).

**Validação de novo, depois das mudanças nas semânticas.** A cache cresceu para **30.244 blocos** (748 mil instruções). O diferencial deu 925.607 casos iguais, e o oráculo, 2.000 blocos iguais com as mesmas 4.381.465.286 instruções. Os testes novos cobrem a lista com parada, a interrupção e a entrega do mutex (40 testes).

### Ferramentas de diagnóstico desta etapa

| Opção | O que faz |
|---|---|
| `--spu-profile` | amostra o PC de cada SPU e mostra os mais quentes no resumo |
| `--verify-zlib` | confere cada descompressão do SPU com o inflate nativo |
| `--dump-mem endereço,tamanho,arquivo` | grava memória do convidado ao parar |
| `--watch-line end[,end...]` | registra cada escrita atômica ou DMA em até 32 linhas de 128 bytes: quem escreveu (thread PPU e LR, ou SPU, tarefa SPURS e `$0`) e as palavras que mudaram |
| `--ppu-profile NOME` | amostra onde a thread `NOME` (PPU, `RSX` ou `SPU1`–`SPU6`) roda no `sly4.exe`, e por DLL fora dele; os endereços se resolvem com `nm`, porque as funções geradas têm o endereço do convidado no nome |
| `--call-times` | tempo dentro de cada chamada HLE e de sistema, por thread e por local de chamada |
| `--rsx-timeline` | as esperas longas do RSX, os *labels* e os *flips*, com tempo absoluto, e as SPUs que acordaram depois de mais de 1 ms paradas |
| `--thread-timeline NOME` | as chamadas da thread PPU `NOME`, com tempo absoluto |

**Desempenho das semânticas** (Fase 5; ver [GPU.md](GPU.md), "Desempenho do menu"):
- As semânticas são `always_inline` nos blocos gerados.
- `shufb` usa `pshufb` (com o laço original como referência e um teste que compara os dois).
- Os *loads*/*stores* de quadword são um `pshufb` (a inversão dos 16 bytes).
- Os denormais da precisão simples são zerados pelo próprio host: DAZ/FTZ do MXCSR, ligados em volta do código SPU (`SpuFloatMode`, em `spu_run` e `spu_run_blocks`). Só o NaN do resultado é normalizado. A precisão dupla desliga os dois (`IeeeDoubles`) e continua IEEE. Os blocos e o interpretador compilam com `-fsignaling-nans`, que impede o GCC de trocar `x * 1.0` por `x`.
- Os laços elemento a elemento vão do último elemento ao primeiro, a ordem da memória do host, e o GCC os vetoriza sem inverter as *lanes*.
- A lógica de 128 bits usa SSE, e os deslocamentos e rotações por bytes usam `pshufb` com máscaras de uma tabela.
- Um bloco já achado fica num cache por SPU (`block_cache`), validado pelas épocas das páginas de 4 KiB do *local store*. Cada escrita (*store* SPU, DMA, `getllar`, kernel) avança a época da página. O acerto é testado no próprio laço, sem chamada.
- O DMA copia com *stores* alinhados de 16 bytes (`dma_copy`), para que outra thread nunca leia um comando pela metade.
- O oráculo do inflate caiu de 7,3 s para 3,9 s, com o diferencial e o oráculo iguais. O menu foi de ~8,6 para ~41–47 quadros/s.
- O projeto compila com `-fno-strict-aliasing`: com o *inline*, o *aliasing* entre os membros da *union* `SpuReg` gerou 123 casos errados.

Além disso:
- O resumo mostra as instruções em volta do PC de cada SPU, e o LR e a cadeia de retornos da pilha de cada thread PPU.
- O log de chamadas inclui as exportações dos módulos do firmware (LLE), marcadas `[LLE]`. Foi assim que apareceu o *workload* do áudio.
