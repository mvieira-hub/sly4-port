"""CRC32 of every compressed block of a PSARC, decompressed with Python's zlib.

Compared with "sly4_recomp psarc-hash" (the port's own inflate) to show that both
decompress the archive to the same bytes (docs/SPU.md, 4f).

    .\tools\run.ps1 psarc_blockhash.py --out /work/psarc_blocks_py.txt
"""
import argparse
import zlib

from psarc import DEFAULT_PSARC, Psarc


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--psarc", default=DEFAULT_PSARC)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    p = Psarc(args.psarc)
    count = 0
    with open(args.out, "w") as out:
        for e in p.entries:
            left, pos, b = e.size, e.offset, e.block
            while left > 0 and b < len(p.blocks):
                stored = p.blocks[b] or p.block_size
                expected = min(p.block_size, left)
                if stored != expected:
                    p.f.seek(pos)
                    data = zlib.decompress(p.f.read(stored))
                    out.write(f"{pos:x} {len(data)} {zlib.crc32(data):08x}\n")
                    count += 1
                left -= expected
                pos += stored
                b += 1
    print(f"{count} blocos comprimidos -> {args.out}")


if __name__ == "__main__":
    main()
