#!/bin/sh
# Reference disassembly of every SPU program extracted by tools/spu_extract.py.
# The ELFs have no section table, so each executable PT_LOAD segment is cut out and
# disassembled as raw SPU code at its local-store address.
# Runs in sly4-spu:1:  docker run --rm -v "${PWD}\work:/work" -v "${PWD}\tools:/t:ro" sly4-spu:1 sh /t/spu/objdump_all.sh
set -e
cd /work/spu
mkdir -p ref
for elf in spu_*.elf; do
    name=${elf%.elf}
    : > "ref/$name.txt"
    # LOAD lines: Type Offset VirtAddr PhysAddr FileSiz MemSiz Flg Align
    # Flags are split by spaces ("R E"), so look for the E field after MemSiz.
    spu-elf-readelf -lW "$elf" | awk '$1 == "LOAD" && ($7 == "E" || $8 == "E" || $7 ~ /E/) {print $2, $3, $5}' | while read -r off vaddr size; do
        dd if="$elf" of="ref/$name.seg" bs=1 skip=$((off)) count=$((size)) 2>/dev/null
        spu-elf-objdump -D -b binary -m spu --adjust-vma="$vaddr" "ref/$name.seg" | grep -E '^ +[0-9a-f]+:' >> "ref/$name.txt"
        rm -f "ref/$name.seg"
    done
    echo "$name: $(wc -l < "ref/$name.txt") instruções"
done
