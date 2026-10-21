"""Splits an spu-elf-objdump listing into functions and summarises each one.

A function is assumed to start right after a return (bi $0 / bi $lr) or at a
branch target of brsl (call). Prints address range, size and instruction mix.
Usage: spu_funcs.py LISTING [--from 0x..] [--to 0x..] [--show 0xADDR]
"""
import argparse
import collections
import re

LINE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*(\S+)\s*(.*)$")


def parse(path):
    out = []
    with open(path) as f:
        for line in f:
            m = LINE.match(line)
            if m:
                out.append((int(m.group(1), 16), m.group(2), m.group(3)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("listing")
    ap.add_argument("--from", dest="lo", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--to", dest="hi", type=lambda x: int(x, 0), default=1 << 32)
    ap.add_argument("--show", type=lambda x: int(x, 0))
    args = ap.parse_args()
    ins = parse(args.listing)
    calls = {int(op.split(",")[-1].split()[0], 16) for _a, mn, op in ins if mn == "brsl" and "0x" in op}
    starts = {ins[0][0]} | calls
    for i, (a, mn, op) in enumerate(ins[:-1]):
        if mn == "bi" and op.strip() in ("$0", "$lr"):
            starts.add(ins[i + 1][0])
    starts = sorted(starts)
    funcs = []
    for k, s in enumerate(starts):
        e = starts[k + 1] if k + 1 < len(starts) else ins[-1][0] + 4
        body = [x for x in ins if s <= x[0] < e]
        funcs.append((s, e, body))
    if args.show is not None:
        for s, e, body in funcs:
            if s <= args.show < e:
                for a, mn, op in body:
                    print(f"{a:6x}: {mn:<10} {op}")
        return
    for s, e, body in funcs:
        if not (args.lo <= s < args.hi) or len(body) < 8:
            continue
        mix = collections.Counter(mn for _a, mn, _o in body)
        bitops = sum(mix[m] for m in ("rotqbi", "rotqbii", "shlqbi", "shlqbii", "rotqmbi", "rotqmbii",
                                      "rotqby", "rotqbybi", "shlqby", "shlqbybi", "rotqmby", "rotqmbybi"))
        andi3 = sum(1 for _a, mn, op in body if mn == "andi" and op.endswith(",3"))
        loops = sum(1 for a, mn, op in body if mn.startswith("br") and "0x" in op
                    and int(op.split(",")[-1].split()[0], 16) < a)
        print(f"{s:6x}-{e:6x} {len(body):5} instr  bitops={bitops:3} andi3={andi3:2} loops={loops:2} "
              f"shufb={mix['shufb']:3} cg={mix['cg'] + mix['clz'] + mix['cntb']:2} calls={mix['brsl']} "
              f"fma={mix['fma'] + mix['fm']:3} cvt={mix['csflt'] + mix['cuflt']:2} "
              f"halfw={mix['ah'] + mix['ahi'] + mix['sfh'] + mix['ceqh'] + mix['ceqhi'] + mix['clgth']:2}")


if __name__ == "__main__":
    main()
