#!/bin/sh
# Mnemonic histogram of the PPU code (EBOOT .text), to size the static recompiler.
#   docker run --rm -v "$PWD/PS3_GAME:/game:ro" -v "$PWD/tools/ppc:/s:ro" sly4-ppc:1 sh /s/opcode_stats.sh [raw]
# With "raw", objdump prints base instructions only (no extended mnemonics such as
# li/mr/beq/clrldi), which is what the recompiler's decoder works with.
# Output: total, distinct mnemonics, then "count mnemonic" sorted.
set -e
ELF=/game/USRDIR/EBOOT.elf
OPTS=cell
[ "$1" = raw ] && OPTS=cell,raw
powerpc64-linux-objdump -d -M "$OPTS" --no-show-raw-insn --start-address=0x10230 --stop-address=0xABD560 "$ELF" \
  | awk -F'\t' 'NF >= 2 && $1 ~ /^ +[0-9a-f]+:$/ { split($2, a, " "); n[a[1]]++; t++ }
      END {
        d = 0; for (m in n) d++
        printf "total %d instrucoes, %d mnemonicos distintos\n", t, d
        for (m in n) printf "%d %s\n", n[m], m
      }' \
  | sort -k1,1nr
