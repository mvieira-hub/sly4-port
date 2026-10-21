# Recompilador estático (Fase 2)

O recompilador lê o `EBOOT.elf` de quem compila e gera C++ a partir do código PowerPC do PPU. Este documento acompanha cada entrega da fase e o modo como ela foi validada.

| Entrega | Estado |
|---|---|
| 2a. Decodificador PPU | **Concluída** (2026-10-21) |
| 2b. Análise: funções, blocos, tabelas de salto | Próxima |
| 2c. Interpretador de referência (semântica de cada instrução) | — |
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
