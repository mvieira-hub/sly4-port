#!/bin/sh
# First occurrence of every base mnemonic in the EBOOT .text, in objdump "raw" syntax,
# with the instruction word: reference examples for the recompiler's disassembler.
#   docker run --rm -v "$PWD/PS3_GAME:/game:ro" -v "$PWD/tools/ppc:/s:ro" sly4-ppc:1 sh /s/raw_samples.sh
set -e
powerpc64-linux-objdump -d -M cell,raw --start-address=0x10230 --stop-address=0xABD560 /game/USRDIR/EBOOT.elf \
  | awk -F'\t' 'NF >= 3 && $1 ~ /^ +[0-9a-f]+:$/ {
        split($3, a, " "); m = a[1]
        if (!(m in seen)) { seen[m] = 1; gsub(/ /, "", $2); print $1 " " $2 "\t" $3 }
      }'
