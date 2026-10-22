"""Compares the recompiler's SPU disassembly with spu-elf-objdump's, address by address.

  objdump side:  tools/spu/objdump_all.sh       -> work/spu/ref/<program>.txt
  our side:      sly4_recomp spu-disasm --elf   -> work/spu/ours/<program>.txt

  compare_disasm.py /work/spu/ref /work/spu/ours [--examples 3]

Every word falls in one of these classes:
  igual             same word and same text (whitespace normalised)
  objdump rejeita   objdump prints .long, we decode an instruction
  nos rejeitamos    we print .long, objdump decodes
  texto diferente   both decode, different text
All classes but "igual" are decoder/disassembler bugs: the SPU disassembler follows
binutils' own lookup, so the expected result is 100 % "igual". Exit code 1 otherwise.
"""
import argparse
import collections
import os
import re
import sys

LINE = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{2} [0-9a-f]{2} [0-9a-f]{2} [0-9a-f]{2}|[0-9a-f]{8})\s+(.*)$")


def load(path):
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.match(line.rstrip("\n"))
            if m:
                out[int(m.group(1), 16)] = (m.group(2).replace(" ", ""), " ".join(m.group(3).split()))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    ap.add_argument("ours")
    ap.add_argument("--examples", type=int, default=3)
    args = ap.parse_args()

    totals = collections.Counter()
    examples = collections.defaultdict(list)
    for name in sorted(os.listdir(args.ref)):
        if not name.endswith(".txt"):
            continue
        ref = load(os.path.join(args.ref, name))
        ours = load(os.path.join(args.ours, name))
        for addr, (word, text) in ref.items():
            if addr not in ours:
                cls = "ausente do nosso lado"
            else:
                oword, otext = ours[addr]
                if oword != word:
                    cls = "palavra diferente"
                elif otext == text:
                    cls = "igual"
                elif text.startswith(".long"):
                    cls = "objdump rejeita"
                elif otext.startswith(".long"):
                    cls = "nos rejeitamos"
                else:
                    cls = "texto diferente"
            totals[cls] += 1
            if cls != "igual":
                key = (cls, text.split()[0] if text else "")
                if len(examples[key]) < args.examples:
                    examples[key].append(f"{name} {addr:x}: objdump [{text}] nosso [{ours.get(addr, ('', ''))[1]}]")
        totals["programas"] += 1

    print(f"{totals['programas']} programas SPU, {sum(v for k, v in totals.items() if k != 'programas')} palavras")
    for cls in ("igual", "objdump rejeita", "nos rejeitamos", "texto diferente", "palavra diferente", "ausente do nosso lado"):
        print(f"  {cls:22} {totals[cls]}")
    for (cls, mnem), lines in sorted(examples.items()):
        print(f"-- {cls} / {mnem}")
        for line in lines:
            print("   " + line)
    bad = sum(v for k, v in totals.items() if k not in ("igual", "programas"))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
