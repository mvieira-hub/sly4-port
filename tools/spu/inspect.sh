#!/bin/sh
# Summary of every extracted SPU program: code/data sections and symbol count.
for f in /work/spu/*.elf; do
  echo "== $f"
  spu-elf-readelf -S -W "$f" 2>/dev/null | grep -E "\.text|\.rodata|\.data " | head -4
  echo "  simbolos FUNC: $(spu-elf-readelf -s -W "$f" 2>/dev/null | grep -c FUNC)"
done
