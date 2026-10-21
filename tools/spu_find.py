"""Finds instruction patterns in spu-elf-objdump listings.

  nibble : rotmi rX,rS,-4 and andi rY,rS,15 within a few instructions (byte split)
Usage: spu_find.py LISTING... [--pattern nibble] [--window 8]
"""
import argparse
import re

LINE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*(\S+)\s*(.*)$")


def parse(path):
    out = []
    with open(path) as f:
        for line in f:
            m = LINE.match(line)
            if m:
                out.append((int(m.group(1), 16), m.group(2), [x.strip() for x in m.group(3).split("#")[0].split(",")]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("listings", nargs="+")
    ap.add_argument("--pattern", default="nibble")
    ap.add_argument("--window", type=int, default=8)
    args = ap.parse_args()
    for path in args.listings:
        ins = parse(path)
        hits = []
        for i, (a, mn, ops) in enumerate(ins):
            if mn == "rotmi" and len(ops) == 3 and ops[2] == "-4":
                src = ops[1]
                for b, mn2, ops2 in ins[max(0, i - args.window):i + args.window]:
                    if mn2 == "andi" and len(ops2) == 3 and ops2[1] == src and ops2[2] == "15":
                        hits.append(a)
                        break
        print(path.split("/")[-1], len(hits), [hex(h) for h in hits[:20]])


if __name__ == "__main__":
    main()
