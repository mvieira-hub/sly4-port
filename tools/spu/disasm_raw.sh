#!/bin/sh
# Disassembles SPU ELFs that have no section headers, segment by segment
# (objdump -D on a raw binary at the segment's load address).
for f in "$@"; do
  base=$(basename "$f" .elf)
  spu-elf-readelf -l -W "$f" | awk '/LOAD/ {print $2, $3, $5, $7 $8}' | while read off vaddr filesz flags; do
    case "$flags" in *E*) ;; *) continue ;; esac
    dd if="$f" of="/tmp/$base.seg" bs=1 skip=$((off)) count=$((filesz)) 2>/dev/null
    spu-elf-objdump -D -b binary -m spu --adjust-vma="$vaddr" "/tmp/$base.seg" > "/work/spu/$base.dis"
    echo "$base: segmento exec off=$off vaddr=$vaddr tam=$filesz -> $(grep -c ':' /work/spu/$base.dis) linhas"
  done
done
