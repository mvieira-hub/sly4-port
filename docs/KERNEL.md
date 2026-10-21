# Kernel e HLE (Fase 3)

O código recompilado do jogo (Fase 2) chama o sistema do PS3 de duas formas: por `sc` (syscalls do kernel LV2, número em `r11`) e pelas funções importadas das bibliotecas do firmware (`cellFs`, `cellSpurs`, `sysPrxForUser`…). Esta fase reimplementa as duas no `sly4.exe`, até o jogo chegar ao `cellGcmInit`.

| Entrega | Estado |
|---|---|
| 3a. Partida do processo: o EBOOT roda do ponto de entrada, com HLE, syscalls e registro das chamadas | **Concluída** (2026-10-21) |
| 3b. Memória, threads PPU e sincronização (lwmutex, mutex, cond, rwlock, semáforo, event flag, filas de evento, timers) | Próxima |
| 3c. Arquivos (VFS, `cellFs` e `sc` 801–818) e módulos de sistema (`cellGame`, `cellSysutil`, `cellSysmodule`, `cellRtc`, stubs de NP e rede) | Pendente |
| 3d. Chegar ao `cellGcmInit`, com o log comparado ao do RPCS3 | Pendente |

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
- **Memória:** `sys_memory_allocate`/`free` em páginas de 64 KiB e 1 MiB, na área `0x20000000`, e o tamanho da memória de usuário (213 MiB, como no PS3).
- **Console:** `sys_tty_write` vai para o log como `[tty0]`.

### Mapa de memória do convidado

Segue as áreas do RPCS3:

| Faixa | Uso |
|---|---|
| `0x00010000–0x011F89B8` | EBOOT (código, dados e `.bss`) |
| `0x01200000–0x1FFFFFFF` | Memória principal: descritores HLE, TLS e, mais tarde, os objetos do kernel |
| `0x20000000–0x2FFFFFFF` | `sys_memory_allocate` |
| `0xD0000000–0xDFFFFFFF` | Pilhas das threads PPU |

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
