#!/bin/sh
# Builds the oracle harness from a directory written by "sly4_recomp oracle-gen"
# (harness.S + cases.bin) and runs it under QEMU as a PowerPC 970 (VMX, 64-bit),
# producing results.bin next to cases.bin.
#   docker run --rm -v "<dir>:/case" -v "<repo>/tools/ppc/oracle:/o:ro" sly4-ppc-oracle:1 sh /o/run.sh
set -e
cd /case
powerpc64-linux-gnu-gcc -O1 -mcpu=970 -maltivec -static -o harness /o/harness_main.c harness.S
qemu-ppc64 -cpu 970 ./harness cases.bin results.bin
echo "ok: $(stat -c %s results.bin) bytes"
