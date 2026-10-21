"""Extracts the SPU ELF programs embedded in the PPU executable.

Every 32-bit big-endian ELF with e_machine 0x17 (SPU) is cut out; its size is
the furthest end of its program segments and section header table.
Usage: spu_extract.py [--elf /game/USRDIR/EBOOT.elf] [--out /work/spu]
"""
import argparse
import os
import re
import struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default="/game/USRDIR/EBOOT.elf")
    ap.add_argument("--out", default="/work/spu")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    with open(args.elf, "rb") as f:
        b = f.read()
    for m in re.finditer(rb"\x7fELF\x01\x02", b):
        o = m.start()
        if struct.unpack_from(">H", b, o + 18)[0] != 0x17:
            continue
        phoff, shoff = struct.unpack_from(">II", b, o + 28)
        phentsize, phnum, shentsize, shnum = struct.unpack_from(">HHHH", b, o + 42)
        end = shoff + shnum * shentsize if shoff else 0
        for i in range(phnum):
            _t, off, _va, _pa, filesz = struct.unpack_from(">IIIII", b, o + phoff + i * phentsize)
            end = max(end, off + filesz)
        path = os.path.join(args.out, f"spu_{o:08X}.elf")
        with open(path, "wb") as f:
            f.write(b[o:o + end])
        print(f"{path}  {end:,} bytes")


if __name__ == "__main__":
    main()
