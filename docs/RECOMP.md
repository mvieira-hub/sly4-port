# Recompilador estático (Fase 2)

O recompilador lê o `EBOOT.elf` de quem compila e gera C++ a partir do código PowerPC do PPU. Este documento acompanha cada entrega da fase e o modo como ela foi validada.

| Entrega | Estado |
|---|---|
| 2a. Decodificador PPU | **Concluída** (2026-10-21) |
| 2b. Análise: funções, blocos, tabelas de salto | **Concluída** (2026-10-21) |
| 2c. Interpretador de referência (semântica de cada instrução) | Próxima |
| 2d. Gerador de C++ + teste diferencial contra o interpretador | — |
| 2e. EBOOT inteiro → C++ compilando | — |

## 2a. Decodificador PPU

Código em `recomp/ppu/`, biblioteca `sly4_ppu`:

| Arquivo | Conteúdo |
|---|---|
| `ppu_ops.inc` | As instruções-base em forma X-macro: enum, mnemônico e "forma" (layout dos operandos). Cobre inteiros, desvios, CR, rotações, FPU, VMX/AltiVec e as instruções exclusivas do Cell (`lvlx`/`lvrx`/`stvlx`/`stvrx` e as variantes `l`, além de `attn`) |
| `ppu_decoder.{h,cpp}` | `decode(word)` → `Insn{op, w}`, com acessores para cada campo (`rd`, `ra`, `simm`, `ds`, `sh64`, `mb64` com os bits trocados, `spr`, `fxm`, `branch_target`…) |
| `ppu_disasm.{h,cpp}` | Texto na sintaxe do `objdump -M cell,raw` |

Detalhes que valem registro:
- **Ordem de decodificação do VMX (opcode 4).** Primeiro a forma VA (6 bits: 32–47); depois a VC (comparações, 10 bits, com os 6 bits baixos = 6 e o `Rc` no bit 21); por último a VX (11 bits). Os valores não colidem.
- **Grupo 31.** As formas XO (9 bits, com OE no bit 21) são testadas antes das X (10 bits). `sradi` é XS (9 bits + `sh[5]`). `mfocrf`/`mtocrf` diferem de `mfcr`/`mtcrf` pelo bit 11.
- **`attn` do Cell:** opcode 0 com XO 256, sem olhar os demais bits (`0x00000200` e `0x00000a00` aparecem no jogo).
- **`mftb`** (31/371) é decodificado. O jogo o usa 1.159 vezes para ler o *time base*; o binutils não o aceita com `-M cell`.
- **Apelidos:** mesmo no modo raw, o objdump mantém alguns. O desmontador os imita para a comparação poder ser exata: `mr`, `not`, `vnot`, `crnot`; `bdnzf`/`bdzf`/`bf`/`bdnzt`/`bdzt`/`bt`/`bdnz`/`bdz` para os BO com dica `at` = 01; e os sufixos `-`/`+` para `at` = 10/11.

### Validação: o `.text` inteiro contra o binutils

```powershell
docker run --rm -v "${PWD}\PS3_GAME:/game:ro" sly4-ppc:1 sh -c "powerpc64-linux-objdump -d -M cell,raw --start-address=0x10230 --stop-address=0xABD560 /game/USRDIR/EBOOT.elf" > work\objdump_raw.txt
.\build\mingw-release\sly4_recomp.exe disasm --eboot PS3_GAME\USRDIR\EBOOT.elf --out work\ours_raw.txt
.\tools\run.ps1 ppc/compare_disasm.py /work/objdump_raw.txt /work/ours_raw.txt
```

Resultado nas 2.795.351 instruções do `.text`:

| Classe | Quantidade |
|---|---|
| Texto idêntico ao objdump | 2.793.207 (99,92%) |
| Mesma instrução, só formatação do objdump (`mfcr rT,-1`; alvos abaixo de 0 em 64 bits) | 419 |
| O objdump rejeita e nós decodificamos | 1.725 |
| **Nós rejeitamos algo que o objdump decodifica** | **0** |
| **Texto diferente** | **0** |

A classe "o objdump rejeita" tem duas origens:
- os 1.159 `mftb`, que são código real;
- palavras de dados no meio do `.text`, onde o binutils confere campos reservados com mais rigor. Por exemplo, `0x42c80000` = 100,0f e `0x42340000` = 45,0f aparecem como `bc`/`bdnz` com BO inválido; `0xffe00000` aparece como `fcmpu`.

Separar código de dado é tarefa da análise de fluxo (2b), não do decodificador.

A decodificação do `.text` inteiro (2,8 milhões de palavras) leva cerca de 1,6 s. O `sly4_tests.exe` traz 64 instruções reais, uma por forma, com o texto esperado do objdump, e roda na CI sem precisar do jogo.

## 2b. Análise de fluxo

Código em `recomp/analysis/program.{h,cpp}`, biblioteca `sly4_analysis`. Para rodar:

```powershell
.\build\mingw-release\sly4_recomp.exe analyze --eboot PS3_GAME\USRDIR\EBOOT.elf --out-dir work\analysis
.\tools\run.ps1 ppc/check_analysis.py --objdump /work/objdump_raw.txt --analysis /work/analysis --eboot-scan /work/eboot/functions.csv
```

**Como funciona.** É uma descida recursiva a partir de pontos de entrada conhecidos:
- os descritores do `.opd`: toda função que tem o endereço tomado no ABI PPC64;
- a entrada do ELF;
- os stubs de importação;
- todo alvo de `bl` que for sendo alcançado.

Cada função é explorada seguindo os desvios. O que nunca é alcançado não vira código, então constantes, tabelas e preenchimento no meio do `.text` ficam de fora sozinhos. Como novas chamadas mudam a classificação dos desvios (tail call para outra função ou desvio local), o processo repete até estabilizar; foram 10 passadas, em 3 s.

| Situação | Classificação |
|---|---|
| `b` para a entrada de outra função | *tail call* |
| `bl` seguido de dado ou de outra função | chamada sem retorno |
| `bcl 20,31,$+4` | só lê o PC; não é chamada |
| `tw`/`twi` com TO = 31 e `attn` | armadilha (fim de bloco) |
| `bclrl`/`bcctrl` | chamada indireta |
| `bcctr` reconhecido | tabela de salto |
| `bcctr` não reconhecido | salto indireto |

**Stubs de importação.** O padrão é `li r12,0 ; oris r12,r12,slot@h ; lwz r12,slot@l(r12) ; … ; bctr`, em que `slot` é um dos *slots* da `.lib.stub`. Cada stub vira uma função do tipo `stub`, com módulo e NID.

**Tabelas de salto.** O padrão do compilador é:
```
cmpli crN,0,rI,K ; bc 12,crN+gt,default ; lwz rB,D(r2) ; rldic/rlwinm rX,rI,2,..
lwzx rY,rX,rB ; extsw rY,rY ; add rZ,rY,rB ; mtctr rZ ; bctr ; .long deslocamentos relativos a rB
```
A busca para trás segue só o código em linha reta: para em qualquer desvio incondicional, e o `cmpli` precisa comparar o registrador de origem do índice. Sem guarda, a tabela termina no primeiro alvo depois da base, porque o código dos casos vem logo após ela. Esse limite também vale para as tabelas com guarda. Uma primeira versão, que atravessava blocos, pegou o `cmpli` errado na função `0x854208`: leu 10 entradas em vez de 19 ou mais e deixou casos de fora.

### Resultado no EBOOT

| Medida | Valor |
|---|---|
| Funções | 23.913 de código (13.858 pelo `.opd`, 10.055 só por chamada) + 251 stubs (uma por importação) |
| Blocos básicos | 300.984 |
| Chamadas diretas / indiretas / tail calls | 97.516 / 24.055 / 400 |
| Tabelas de salto | 178 (todos os `bcctr` incondicionais do jogo): 127 com guarda confirmado, 51 delimitadas pelo primeiro alvo; base na TOC sempre igual a `bctr+4` |
| Saltos indiretos não resolvidos | 251, exatamente os stubs |
| Instruções inválidas alcançadas | **0** |
| Código | 2.770.846 palavras (99,14% das não nulas); 4.430 de tabelas |

**Conferências independentes** (`check_analysis.py`):
- os 1.159 `mftb` estão todos no código;
- **nenhuma das 8.885 outras palavras que o binutils rejeita está no código**: 4.398 são entradas de tabela e 4.487 não são alcançadas;
- 24.147 entradas coincidem com o `eboot_scan.py`. As 25 que só ele tem são `bl` achados por varredura linear em área não alcançada. As 17 que só nós temos são stubs de importações que o jogo nunca chama.

**O que sobra não alcançado** são 19.557 palavras não nulas. Nos 144 trechos de 8 palavras ou mais que decodificam quase inteiros:
- 60 são grupos de constantes `float`;
- 14 são pares `nop; b` mortos;
- 69 são fragmentos mortos com `blr`: epílogos soltos e funções minúsculas que o compilador expandiu em todo lugar;
- 1 é misto.

Nenhum tem prólogo de função (`stdu r1` / `mflr`). Mesmo assim, o runtime vai ter um interpretador (2c) como rede de segurança para qualquer destino indireto que a análise não conheça.

O `sly4_tests.exe` monta à mão um programa com `switch` por tabela, chamada, tail call e `float` no meio do código, e confere a análise na CI.
