# Kernel e HLE (Fase 3)

O código recompilado do jogo (Fase 2) chama o sistema do PS3 de duas formas: por `sc` (syscalls do kernel LV2, número em `r11`) e pelas funções importadas das bibliotecas do firmware (`cellFs`, `cellSpurs`, `sysPrxForUser`…). Esta fase reimplementa as duas no `sly4.exe`, até o jogo chegar ao `cellGcmInit`.

| Entrega | Estado |
|---|---|
| 3a. Partida do processo: o EBOOT roda do ponto de entrada, com HLE, syscalls e registro das chamadas | **Concluída** (2026-10-21) |
| 3b. Memória, threads PPU e sincronização (lwmutex, mutex, cond, rwlock, semáforo, event flag, filas de evento, timers) | **Concluída** (2026-10-21) |
| 3c. Arquivos (VFS, `cellFs` e `sc` 801–818) e módulos de sistema (`cellGame`, `cellSysutil`, `cellSysmodule`, `cellRtc`, stubs de NP e rede) | **Concluída** (2026-10-21) |
| 3d. Chegar ao `cellGcmInit`: SPURS mínimo e a tarefa SPU de descompressão (EDGE zlib) em código nativo | **Concluída** (2026-10-21), com o TTY idêntico ao do RPCS3 |

Código em `runtime/kernel/`, biblioteca `sly4_kernel`.

## 3a. Partida do processo

```powershell
.\build\mingw-release\sly4.exe --game PS3_GAME --no-window --seconds 20
```

O jogo só roda quando o build inclui o código gerado (`generated/ppu`, ver [RECOMP.md](RECOMP.md)). Sem ele, o `sly4.exe` carrega o EBOOT e avisa.

Opções:
- `--no-window`: roda o jogo sem abrir a janela;
- `--seconds N`: para o jogo depois de N segundos;
- `--trace`: registra todas as chamadas, e não só a primeira de cada função;
- `--ring N`: guarda as chamadas num anel em memória e imprime as últimas N no resumo. Serve para investigar travamentos sem mudar o tempo de execução, como o `--trace` muda;
- `--data <pasta>`: onde ficam `/dev_hdd0` e `/dev_hdd1`. O padrão é `%LOCALAPPDATA%\sly4-port`;
- `--no-run`: só carrega o EBOOT.

No fim, o programa lista quantas vezes cada função e cada syscall foi chamada, e marca as que ainda não têm implementação.

### Thread principal

A partida segue o que o RPCS3 faz em `ppu_load_exec`:

| Registrador | Valor |
|---|---|
| `r1` | Topo da pilha − argumentos − 0x70. A pilha tem o tamanho do `sys_process_param` (64 KiB neste jogo), fica em `0xD0000000+` e tem uma página de guarda abaixo |
| `r2` | TOC (`0xF50800`) |
| `r3`–`r6` | `argc`, `argv`, `envp`, `envc`. `argv[0]` = `/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN`, com ponteiros de 64 bits no topo da pilha |
| `r7` | ID da thread (`0x01000000`) |
| `r8`–`r10` | Imagem, tamanho inicial e tamanho total do TLS (`PT_TLS`) |
| `r11` | Descritor de entrada (`e_entry`) |
| `r12` | Página do malloc (`sys_process_param`, 1 MiB) |
| LR | Endereço de "retorno de thread": voltar para ele encerra a thread |

O `_start` do jogo (`0x4FC7B4`) usa exatamente esses registradores: copia `argv`/`envp` para ponteiros de 32 bits e chama `sys_initialize_tls` com `r7`–`r10`. Essa função cria o bloco de TLS da thread: 0x30 bytes de área do sistema, depois a imagem e o resto zerado. `r13` fica 0x7000 além do começo dos dados, o deslocamento do ABI PPC64.

O jogo roda numa thread do Windows com 256 MiB de pilha reservada, porque cada chamada do convidado vira uma chamada C++. `sys_process_exit` e `sys_ppu_thread_exit` lançam uma exceção C++ que desenrola a pilha do convidado até o laço da thread.

### Funções importadas (HLE)

O loader do PS3 preenche cada *slot* de importação com o endereço do descritor `{função, TOC}` exportado pelo firmware. O stub do jogo (`.lib.stub`, `0xABF2A4…`) lê o slot e salta com `bctr`.

Aqui, cada uma das 251 importações recebe um descritor numa área própria (`0x01200000`, logo depois da imagem). A "função" desse descritor é um endereço onde não há código. `rt::call_indirect`, usado pelo código gerado, e o interpretador reconhecem essa faixa: rodam a função nativa e voltam para o LR, como um `blr`.

Uma importação sem implementação devolve `CELL_OK` e tem a primeira chamada registrada, com os argumentos e o endereço de quem chamou. Assim cada execução mostra a próxima peça que falta. O nome vem do NID: das implementações registradas ou da lista `tools/data/ps3_nid_candidates.txt`.

### Syscalls

O `sc` vai para `syscall_dispatch`. A tabela tem os nomes de todas as 71 syscalls que o jogo chama diretamente, conferidos com o `lv2.cpp` do RPCS3. Como no HLE, a primeira chamada de cada uma é registrada, e as que não têm implementação devolvem `CELL_OK`.

Implementadas na 3a:
- **Processo e thread:** `getpid`, `_sys_process_exit`, `yield`, prioridade e pilha da thread.
- **Tempo:** `usleep`, `sleep`, fuso, hora atual e frequência do *timebase* (79,8 MHz).
- **Memória:** `sys_memory_allocate`/`free` em páginas de 64 KiB e 1 MiB e o tamanho da memória de usuário (213 MiB, como no PS3). Os ~18,6 MiB que o firmware já ocupa ao iniciar o jogo contam como usados.
- **Console:** `sys_tty_write` vai para o log como `[tty0]`.

### Mapa de memória do convidado

Segue as áreas do RPCS3:

| Faixa | Uso |
|---|---|
| `0x00010000–0x011F89B8` | EBOOT (código, dados e `.bss`) |
| `0x01200000–0x0FFFFFFF` | Memória principal: descritores HLE, TLS e objetos do kernel |
| `0x10000000–0x1FFFFFFF` | `sys_memory_allocate` em páginas de 64 KiB. As primeiras 0xA0000 são do firmware, como no RPCS3 |
| `0x30000000–0x3FFFFFFF` | `sys_memory_allocate` em páginas de 1 MiB |
| `0x40000000–0xBFFFFFFF` | Faixas do `sys_mmapper` (3b) |
| `0xD0000000–0xDFFFFFFF` | Pilhas das threads PPU |

Os endereços acompanham os do RPCS3. Assim os ponteiros que aparecem nos logs e no TTY são os mesmos nos dois (ver 3d).

### Resultado

Em 20 s de execução, o jogo:
1. roda o `crt0`, monta o TLS e inicializa o próprio alocador (`sys_memory_allocate` de 1 MiB);
2. imprime no TTY o mapa do executável (`Versions:SNC:41001`, código, dados e `.bss`) e a memória de usuário (213 MB);
3. registra os *callbacks* de sistema, chama `sys_spu_initialize(6)` e configura o SPURS: atributos, *job chains*, *event flags* e *job guards*;
4. passa por `cellGameBootCheck`, `cellGameGetParamInt`/`String`, `cellGameContentPermit` e `cellSysCacheMount`, imprimindo cada resultado no TTY;
5. para esperando numa variável de condição (`wait for invalid cond 'opWait'`). Ela devia ser sinalizada pela thread de E/S que o `sys_ppu_thread_create` ainda não cria, e é a entrega 3b.

No total, foram chamadas 32 funções HLE (26 ainda sem implementação) e 10 syscalls. O próprio jogo ajuda: ele avisa no TTY cada mutex ou condição inválida (`attempt to lock invalid mutex 's_mediaListLock'`).

Testes no `sly4_tests.exe` (`test_kernel.cpp`):
- o alocador: *first fit*, junção de blocos livres e recusa de liberação dupla;
- um stub de importação idêntico aos do EBOOT, que chega à função nativa pelo descritor e volta para o LR.

## 3b. Threads, sincronização e memória

### Threads PPU

`sys_ppu_thread_create` (no `sysPrxForUser`) cria a thread com:
- pilha própria com página de guarda;
- bloco de TLS copiado da imagem que o `crt0` registrou;
- `r2` do descritor de entrada, `r3` = argumento e LR = "retorno de thread".

Cada thread roda numa thread do Windows com 64 MiB de pilha reservada e o nome do PS3, que aparece no depurador. O retorno da função de entrada ou `sys_ppu_thread_exit` encerra a thread. `join` (`sc` 44) espera e devolve o valor de saída, e `detach` (45) libera a thread.

`sys_ppu_thread_once` chama a função de inicialização do convidado a partir do código nativo (`call_guest`). Ela ganha um quadro de pilha novo abaixo da zona protegida de quem chamou, e LR, `r1` e `r2` são restaurados na volta.

Um `sys_process_exit` em qualquer thread encerra o processo.

### Sincronização

`runtime/kernel/sync.h` implementa os objetos do LV2 sobre `std::mutex` e `std::condition_variable`. Cada objeto fica num gerenciador de IDs, com as mesmas faixas que o RPCS3 usa (`0x85…` mutex, `0x8D…` fila de evento etc.).

| Objeto | Syscalls / funções | Semântica |
|---|---|---|
| mutex e lwmutex | 100–104; `sys_lwmutex_*` | Recursivo ou não (`EDEADLK`), `EPERM` para quem não é dono, `EBUSY` no `trylock`, *timeout* em µs |
| cond e lwcond | 105–109; `sys_lwcond_*` | A espera solta o mutex por completo (mesmo recursivo), espera um sinal e pega o mutex de volta com a mesma profundidade. Sinal sem ninguém esperando se perde, como no LV2 |
| rwlock | 120–127 | Vários leitores ou um escritor |
| semáforo | 90–94, 114 | `EBUSY` acima do máximo |
| event flag | 82–87, 118 | Espera AND/OR com limpeza opcional; `clear` faz `bits &= padrão`, como o LV2 |
| fila e porta de evento | 128–138 | Fila de até 127 eventos. `receive` devolve o evento em `r4`–`r7`. A origem é o nome da porta ou `(pid << 32) \| id` |
| timer | 70–76 | Uma thread do Windows por timer iniciado, que envia `{nome, dado1, dado2, horário}` à fila ligada |

O lwmutex mantém atualizados no convidado os campos `owner` e `recursive_count` da `sys_lwmutex_t`, para o código que os consulta direto. O estado que vale é o do objeto nativo. Prioridades e protocolos (FIFO/prioridade) não são modelados: quem acorda é decidido pelo escalonador do Windows.

### Memória

- `sys_memory_allocate`/`free` (348/349): páginas de 64 KiB ou 1 MiB.
- `get_page_attribute` (351) e o total de 213 MiB (352), que agora conta também os blocos do mmapper.
- `sys_mmapper_allocate_address`/`free_address` (330/331): reservam faixas de 256 MiB em `0x40000000–0xBFFFFFFF`.
- Os *wrappers* `sys_mmapper_allocate/map/unmap/free_memory` do `sysPrxForUser` mapeiam blocos dentro dessas faixas.

### Resultado

Com threads e sincronização, o jogo vai bem mais longe:
1. cria 5 threads: `_SPU_printf_server` (espera numa fila de evento), `ApeWorkerThread_1` (espera num event flag), `fios mediathread 2` e `3` e `fios scheduler 1`;
2. o FIOS, a biblioteca de arquivos da Sony embutida no jogo, sobe e troca sinais entre as threads por lwcond e rwlock. Os avisos `attempt to lock invalid mutex` da 3a sumiram;
3. a thread de mídia faz as primeiras chamadas de arquivo (`cellFsStat` e `cellFsMkdir`, ainda sem implementação);
4. a thread principal lê o endereço 0 logo depois (LR do convidado `0x5E89C4`).

A queda vem do que ainda responde "OK" sem preencher nada. `cellGameBootCheck` e `cellGameContentPermit` deixam lixo nas variáveis de saída (`get_type = [1869633377]`), e o `cellFs` não existe ainda. Isso é a entrega 3c.

Testes novos no `sly4_tests.exe`, com threads de verdade:
- mutex: recursão, `EDEADLK`/`EPERM`/`EBUSY` e *timeout*;
- condição: a espera solta um mutex recursivo, outra thread o pega e sinaliza, e a profundidade volta igual;
- fila de evento: cheia, vazia com *timeout* e recepção bloqueante;
- event flag: AND/OR com limpeza;
- semáforo: limite máximo.

## 3c. Arquivos e módulos de sistema

### VFS

| Caminho do PS3 | No PC |
|---|---|
| `/dev_bdvd/...` | A pasta que contém o `PS3_GAME` do dump (`--game`) |
| `/app_home/...` | `PS3_GAME/USRDIR` |
| `/dev_hdd0/...` | `%LOCALAPPDATA%\sly4-port\hdd0` (ou `--data <pasta>`) |
| `/dev_hdd1/...` | `...\hdd1`. Depois do `cellSysCacheMount`, passa a ser `hdd1\caches\<TITLE_ID>_<id do cache>\`, esvaziada a cada montagem, como no RPCS3 |

Os caminhos são normalizados (`//`, `.`, `..`) e nunca saem do dispositivo. O nome do dispositivo ignora maiúsculas, e o Windows já ignora no resto.

### cellFs e syscalls 801–818

Uma tabela de descritores (a partir de 3) sobre `CreateFileW`, compartilhada por `cellFs*` e pelos `sc` diretos:
- open (flags `O_CREAT`/`EXCL`/`TRUNC`/`APPEND`), read, write, close, lseek, fsync;
- stat e fstat (`CellFsStat` de 52 bytes);
- opendir, readdir (com `.` e `..`) e closedir, mais `cellFsGetDirectoryEntries`;
- mkdir, rmdir, unlink e `cellFsGetFreeSize` (40 GiB livres);
- `cellFsAioRead`/`Write` numa thread de sistema `FsAio`, que chama o *callback* do jogo pelo `call_guest`.

Os erros seguem os códigos do PS3 (`ENOENT`, `EEXIST`, `EBADF`, `ENOTEMPTY`…). Cada `open`/`stat`/`mkdir` vai para o log como `[fs]`, para comparar com o RPCS3.

### Módulos

| Módulo | O que responde |
|---|---|
| `cellGame` | Jogo em disco: tipo DISC, 40 GiB livres e tamanho "não calculado". `ContentPermit` devolve `/dev_bdvd/PS3_GAME` e `.../USRDIR`. `GetParamInt`/`String` leem o `PARAM.SFO` do dump. `PatchCheck` responde `NOTPATCH` |
| `cellSysutil` | *Callbacks* registrados (sem eventos ainda); parâmetros do sistema (inglês, X confirma, 24 h); vídeo 1280×720 16:9 a 59,94 Hz; áudio estéreo LPCM; `cellSysCacheMount`; diálogos só no log |
| `cellSysutilAvconfExt` | Tela de 40", gama |
| `cellSysmodule` | Carregar e descarregar módulos só marca o estado |
| `cellRtc` | Hora local do Windows |
| `cellNetCtl`, `sys_net` | Rede desconectada; `_sys_net_errno_loc` |

### Resultado

O jogo agora escreve no TTY os valores reais:
- `CELL_GAME_GAMETYPE_DISC`;
- título "Sly Cooper: Thieves in Time™" e `TITLE_ID` BCES01284;
- `PARENTAL_LEVEL` 3;
- os caminhos `/dev_bdvd/PS3_GAME/USRDIR`.

Não há mais queda. O FIOS:
1. monta o cache e cria `/dev_hdd1/dev_hdd1/cache.idx` e `cache.dat`;
2. sobe mais 4 threads de mídia;
3. abre o `Sly4.psarc`, lê o cabeçalho e o índice (cerca de 3 MB) e copia blocos de dados para o cache com duas threads em paralelo.

Para a primeira leitura comprimida, o FIOS manda um pedido para uma **tarefa SPU do SPURS** (`_cellSpursLFQueuePushBody`; a tarefa foi criada com `cellSpursCreateTask` a partir do ELF SPU em `0xE42D00`). Depois espera o término em `cellSpursEventFlagWait`. Sem SPURS, ninguém descomprime e a espera não acaba.

O próximo bloqueio é, portanto, o SPU, e não mais o kernel ou os arquivos.

O resumo final agora também lista cada thread com a última chamada que fez, o que mostra onde cada uma está esperando.

Teste novo no `sly4_tests.exe`, sobre uma pasta temporária com a estrutura de um dump:
- resolução de caminhos, inclusive a recusa de sair do dispositivo;
- leitura com `lseek` e `read_at`, e `stat`/`fstat`;
- criação e escrita em `/dev_hdd0`;
- listagem de pasta e os erros `EEXIST`, `EBADF`, `ENOENT` e `ENOTEMPTY`.

## 3d. Até o `cellGcmInit`

O FIOS lê o `Sly4.psarc` em blocos de 64 KiB comprimidos com zlib, e quem descomprime é uma **tarefa SPU do SPURS**. Sem SPU (Fase 4), o boot parava na primeira leitura comprimida. Seguindo o "atalho opcional" do roteiro, a tarefa foi trocada por código nativo, e o resto do SPURS que o FIOS usa foi implementado no PPU.

### Identificação da tarefa

- `cellSpursCreateTask` recebe o ELF SPU em `0xE42D00`, de 38.100 bytes. É o `spu_00E32D00.elf` do `tools/spu_extract.py`.
- As mensagens do próprio ELF identificam a tarefa como `edgezlib_inflate_task.cpp`, o EDGE zlib da Sony com zlib 1.2.3 para SPU.
- O runtime reconhece a tarefa pelo hash FNV-1a do ELF (`B28A89DF3EB9235C`). Uma tarefa desconhecida só é registrada no log.

### Protocolo

O elemento de 32 bytes da fila foi deduzido de três fontes:
- o código PPU do FIOS que o monta (`0x60DA10`);
- as mensagens de erro da tarefa (`outputUncompSkipBeginSize + outputUncompSkipEndSize > expectedUncompSize`, "decrement a work-to-do counter");
- os pedidos observados em execução (`--trace` imprime cada elemento empurrado na fila).

| Offset | Campo |
|---|---|
| +0 | Origem: DEFLATE puro. Quem chama já pula os 2 bytes do cabeçalho zlib |
| +4 | Destino |
| +8 | Tamanho comprimido |
| +12 | Bytes a entregar |
| +16 | Contador de trabalho pendente, com flags nos 2 bits baixos. Com o bit 0, a tarefa decrementa o contador; sem ele, grava ali o tamanho entregue |
| +20 | `CellSpursEventFlag` a sinalizar |
| +24 | Bits do event flag (`1 << slot`) |
| +26 / +28 | Bytes a pular no início e no fim do bloco descomprimido |

O bloco descomprime para `pular_início + entregar + pular_fim` bytes. Só a janela do meio vai para o destino. O FIOS espera em `cellSpursEventFlagWait` até o contador (o campo `+40` do seu status de 44 bytes) zerar.

### Implementação

- `runtime/core/inflate.cpp`: decodificador DEFLATE próprio (RFC 1951), sem dependências. Tem blocos armazenados, Huffman fixo e dinâmico, tabela direta de 10 bits e o percurso canônico para códigos mais longos.
- `kernel/hle_cell_spurs.cpp`:
  - event flags do SPURS (16 bits, AND/OR, limpeza automática);
  - LFQueue (entradas de tamanho fixo, profundidade, push/pop bloqueantes);
  - `cellSpursCreateTask`.
- `kernel/spurs_tasks.cpp`: a tarefa EDGE zlib numa thread do Windows. Ela tira pedidos da fila, descomprime, copia a janela, decrementa o contador (CAS em big-endian) e liga os bits do event flag.

### Bug encontrado no caminho: sinal "roubado" no `signal_all`

Sem `--trace` o boot travava; com ele, que deixa tudo mais lento, andava. Para investigar sem mudar o tempo, o `--ring N` guarda as últimas chamadas num anel em memória e só as imprime no resumo. O anel mostrou a sequência:
1. duas threads esperavam a mesma condição, e `signal_all` liberava 2 sinais;
2. a thread principal acordava, voltava a esperar na mesma hora e consumia o segundo sinal;
3. a `fios mediathread 5`, dona desse segundo sinal, nunca acordava.

A condição agora usa senhas em ordem de chegada: cada espera pega um número, `signal` libera o mais antigo ainda esperando e `signal_all` libera todos os emitidos até ali. Quem volta a esperar pega um número novo. O teste `sync_cond_signal_all_is_not_stolen` reproduz o cenário.

### Resultado

Em 3 execuções seguidas, o jogo:
1. descomprime o primeiro bloco do `Sly4.psarc` com exatamente o tamanho esperado (52.894 → 65.536 bytes);
2. termina a inicialização do FIOS;
3. **chega ao `cellGcmInit`**: `cellGcmInitDefaultFifoMode`, `_cellGcmInitBody` (buffer de comandos de 0x11000 bytes e área de IO de 8 MiB em `0x30B00000`), `cellGcmGetConfiguration`, `cellGcmMapMainMemory` (25 MiB em `0x31400000`), `cellGcmSetVBlankFrequency` e `cellGcmSetVBlankHandler`;
4. cai em seguida lendo o endereço 4, depois de `cellGcmGetTiledPitchSize`. O `cellGcm` ainda não existe e é a Fase 5.

Testes novos:
- `inflate_raw_blocks`: bloco armazenado, bloco de Huffman fixo com cópia para trás, montados à mão a partir da RFC, e os erros de saída pequena, entrada truncada e tipo de bloco inválido;
- `sync_cond_signal_all_is_not_stolen`.

### Comparação com o RPCS3

O firmware que veio no disco (`PS3_UPDATE/PS3UPDAT.PUP`) foi instalado no RPCS3 local (v0.0.43) com `rpcs3.exe --installfw`. Depois o jogo rodou nele uma vez por 75 s com `--no-gui`. A comparação usa o `RPCS3.log` e o `TTY.log` e cobre o trecho até o `cellGcmInit`. No RPCS3, `liblv2`, `libfs`, `libgcm_sys` e o SPURS rodam como código real do firmware, então a comparação é feita pelo que aparece nos dois: TTY, syscalls e arquivos.

| O quê | Resultado |
|---|---|
| TTY do jogo | **Idêntico, linha a linha**, nas 30 linhas que o sly4 produz até o `cellGcmInit`. A linha seguinte do RPCS3 ("APE_DEBUG enabled in APE_FINAL") já vem depois dele |
| Arquivos | Mesma sequência: `stat` e `mkdir` de `/dev_hdd1/dev_hdd1`, `cache.idx` e `cache.dat` (descritores 3 e 4), e duas aberturas do `Sly4.psarc` (descritor 5) com dois `stat` cada |
| Ordem dos eventos | Mesma: cache, `cellPadInit`, `sceNpInit`, psarc e então a alocação do RSX (`_cellGcmInitBody`). Muda só a intercalação entre a thread principal e as do FIOS |
| Endereços | Iguais depois do ajuste: memória livre de 194 MB, alocação de `0xBD00000` em `0x30100000`, pool do `sceNpInit` em `0x306D0400` e área de IO do GCM em `0x30B00000` |

Duas diferenças apareceram e foram corrigidas:
- **Mapa de memória:** o RPCS3, como o PS3, desconta a memória que o firmware já ocupa (o jogo via 213 MB livres em vez de 194) e põe as páginas de 1 MiB em `0x30000000`. Agora o sly4 faz igual.
- **Resoluções 3D:** `cellVideoOutGetResolution(0x81)` devolve 1280×1470, como no RPCS3. O jogo imprime "3D Video mode configured as 1280x1470".

Para repetir: rodar o jogo no RPCS3, copiar `log\RPCS3.log` e `log\TTY.log` e comparar com a saída do `sly4.exe` (o `TTY` vem nas linhas `[tty0]`).
