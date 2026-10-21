"""Inventory of a PSARC by top folder and extension, plus the magic bytes of each
extension (first file of each kind), to know which formats still need a decoder.

  psarc_inventory.py /game/USRDIR/Sly4.psarc
"""
import sys
from collections import defaultdict

from psarc import Psarc


def ext(name):
    n = name.lower().rsplit("/", 1)[-1]
    if n.endswith(".san.cooked"):
        return "san.cooked"
    return n.rsplit(".", 1)[-1] if "." in n else "(sem)"


def main():
    p = Psarc(sys.argv[1])
    by_ext = defaultdict(lambda: [0, 0, None])
    by_dir = defaultdict(lambda: [0, 0])
    for e in p.files:
        k = ext(e.name)
        by_ext[k][0] += 1
        by_ext[k][1] += e.size
        if by_ext[k][2] is None:
            by_ext[k][2] = e
        top = e.name.lstrip("/").split("/", 1)[0]
        by_dir[top][0] += 1
        by_dir[top][1] += e.size
    print(f"{len(p.files)} arquivos, {sum(v[1] for v in by_ext.values()) / 2**30:.2f} GiB descomprimidos\n")
    print("pasta raiz:")
    for k, (n, sz) in sorted(by_dir.items(), key=lambda kv: -kv[1][1]):
        print(f"  {n:7d}  {sz / 2**20:10.1f} MiB  {k}")
    print("\nextensao:")
    for k, (n, sz, first) in sorted(by_ext.items(), key=lambda kv: -kv[1][1]):
        magic = p.read(first, limit=16)[:8]
        print(f"  {n:7d}  {sz / 2**20:10.1f} MiB  {k:12s} {magic.hex(' '):24s} {magic!r:22s} ex: {first.name}")


if __name__ == "__main__":
    main()
