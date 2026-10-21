"""Reader/extractor for Sony PSARC archives (Sly4.psarc).

Layout (all big-endian):
  header 32 bytes: 'PSAR', version, compression ('zlib'), toc_length,
                   toc_entry_size (30), toc_entries, block_size, flags
  toc entries:     md5[16], first_block u32, size u40, offset u40
  block table:     one u16 per block (block_size 64K) with the stored length;
                   0 means a full raw block.
Entry 0 is the manifest: the newline-separated file names of entries 1..N.

Usage:
  psarc.py list    [--psarc P] [--filter GLOB ...]
  psarc.py extract [--psarc P] [--filter GLOB ...] [--out DIR]
"""
import argparse
import fnmatch
import os
import struct
import sys
import zlib

DEFAULT_PSARC = "/game/USRDIR/Sly4.psarc"


class Entry:
    __slots__ = ("index", "block", "size", "offset", "name")

    def __init__(self, index, block, size, offset):
        self.index, self.block, self.size, self.offset, self.name = index, block, size, offset, None


class Psarc:
    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(32)
        magic, ver, comp, toc_len, ent_size, count, self.block_size, self.flags = struct.unpack(">4sI4sIIIII", head)
        if magic != b"PSAR":
            raise ValueError(f"{path}: not a PSARC")
        if comp != b"zlib":
            raise ValueError(f"unsupported compression {comp!r}")
        toc = self.f.read(toc_len - 32)
        self.entries = []
        for i in range(count):
            o = i * ent_size
            block = struct.unpack_from(">I", toc, o + 16)[0]
            size = int.from_bytes(toc[o + 20:o + 25], "big")
            offset = int.from_bytes(toc[o + 25:o + 30], "big")
            self.entries.append(Entry(i, block, size, offset))
        table = toc[count * ent_size:]
        width = 2 if self.block_size <= 0x10000 else 3 if self.block_size <= 0x1000000 else 4
        self.blocks = [int.from_bytes(table[i:i + width], "big") for i in range(0, len(table) - width + 1, width)]
        names = self.read(self.entries[0]).decode("utf-8").split("\n")
        for e, n in zip(self.entries[1:], names):
            e.name = n.strip()
        self.files = [e for e in self.entries[1:] if e.name]

    def iter_chunks(self, entry):
        """Yields the decompressed data of an entry, one block at a time."""
        left, pos, b = entry.size, entry.offset, entry.block
        while left > 0:
            stored = self.blocks[b] or self.block_size
            expected = min(self.block_size, left)
            self.f.seek(pos)
            raw = self.f.read(stored)
            data = raw if stored == expected else zlib.decompress(raw)
            yield data
            left -= len(data)
            pos += stored
            b += 1

    def read(self, entry, limit=None):
        out = bytearray()
        for chunk in self.iter_chunks(entry):
            out += chunk
            if limit is not None and len(out) >= limit:
                return bytes(out[:limit])
        return bytes(out)

    def find(self, name):
        name = name.lower()
        return next((e for e in self.files if e.name.lower() == name), None)

    def select(self, patterns):
        if not patterns:
            return list(self.files)
        pats = [p.lower() for p in patterns]
        return [e for e in self.files if any(fnmatch.fnmatch(e.name.lower(), p) for p in pats)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["list", "extract"])
    ap.add_argument("--psarc", default=DEFAULT_PSARC)
    ap.add_argument("--filter", action="append", default=[], help="glob on the internal path, e.g. '*.xmb' (repeatable)")
    ap.add_argument("--out", default="/work/psarc")
    args = ap.parse_args()

    arc = Psarc(args.psarc)
    files = arc.select(args.filter)
    if args.command == "list":
        for e in files:
            print(f"{e.size:>12}  {e.name}")
        print(f"{len(files)} arquivos, {sum(e.size for e in files) / 2**20:.1f} MB", file=sys.stderr)
        return

    total = sum(e.size for e in files)
    done = 0
    for n, e in enumerate(files, 1):
        dest = os.path.join(args.out, e.name.lstrip("/"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if os.path.exists(dest) and os.path.getsize(dest) == e.size:
            done += e.size
            continue
        with open(dest + ".part", "wb") as w:
            for chunk in arc.iter_chunks(e):
                w.write(chunk)
        os.replace(dest + ".part", dest)
        done += e.size
        if n % 200 == 0 or e.size > 50 * 2**20:
            print(f"[{n}/{len(files)}] {done / 2**20:,.0f}/{total / 2**20:,.0f} MB  {e.name}", flush=True)
    print(f"OK: {len(files)} arquivos em {args.out}")


if __name__ == "__main__":
    main()
