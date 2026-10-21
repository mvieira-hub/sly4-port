#!/bin/sh
# Disassembles a raw byte range of a file as SPU code.
#   disasm_range.sh FILE START END OUT   (START/END are file offsets, hex ok)
f=$1; start=$(($2)); end=$(($3)); out=$4
dd if="$f" of=/tmp/range.bin bs=4096 skip=$((start / 4096)) count=$(((end - start + 4095) / 4096)) 2>/dev/null
spu-elf-objdump -D -b binary -m spu --adjust-vma="$start" /tmp/range.bin > "$out"
echo "$out: $(grep -c ':' "$out") linhas"
