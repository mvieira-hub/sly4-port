"""Compares the recompiler's disassembly with GNU objdump's, address by address.

  objdump side:  powerpc64-linux-objdump -d -M cell,raw --start-address=... (tools/ppc image)
  our side:      sly4_recomp disasm --eboot ... --out ours.txt

  compare_disasm.py objdump.txt ours.txt [--examples 3]

Every address falls in one of these classes:
  igual                 same word and same text (whitespace normalised)
  formato aceito        same instruction, known objdump formatting quirks:
                        "mfcr rT,-1", and branch targets below 0 printed as 64-bit
  objdump rejeita       objdump prints .long, we decode an instruction. Expected for
                        data words inside .text (float constants, tables) where binutils
                        checks reserved fields more strictly; mftb (31/371) is real Cell
                        code that binutils does not accept with -M cell
  nos rejeitamos        we print .long, objdump decodes: a decoder bug (must be 0)
  texto diferente       both decode, different text: a decoder/disassembler bug (must be 0)
The exit code is 1 when either of the last two classes is not empty.
"""
import argparse
import collections
import re
import sys

LINE = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{2} [0-9a-f]{2} [0-9a-f]{2} [0-9a-f]{2}|[0-9a-f]{8})\s+(.*)$")
WIDE_TARGET = re.compile(r"0xffffffff([0-9a-f]{8})$")


def load(path):
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.match(line.rstrip("\n"))
            if m:
                text = " ".join(m.group(3).split())
                out[int(m.group(1), 16)] = (m.group(2).replace(" ", ""), text)
    return out


def normalise_objdump(text):
    if text.startswith("mfcr ") and text.endswith(",-1"):
        return text[:-3]
    return WIDE_TARGET.sub(lambda m: "0x" + m.group(1).lstrip("0"), text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("objdump")
    ap.add_argument("ours")
    ap.add_argument("--examples", type=int, default=3)
    args = ap.parse_args()
    ref, ours = load(args.objdump), load(args.ours)
    classes = collections.Counter()
    groups = collections.defaultdict(list)
    for addr, (word, text) in sorted(ref.items()):
        mine = ours.get(addr)
        if mine is None or mine[0] != word:
            cls = "palavra ausente/diferente"
            key = (cls, "", "")
        elif mine[1] == text:
            classes["igual"] += 1
            continue
        elif mine[1] == normalise_objdump(text):
            cls, key = "formato aceito", ("formato aceito", text.split()[0], "")
        elif text.startswith(".long"):
            cls, key = "objdump rejeita", ("objdump rejeita", ".long", mine[1].split()[0])
        elif mine[1].startswith(".long"):
            cls, key = "nos rejeitamos", ("nos rejeitamos", text.split()[0], ".long")
        else:
            cls, key = "texto diferente", ("texto diferente", text.split()[0], mine[1].split()[0])
        classes[cls] += 1
        groups[key].append((addr, word, text, mine[1] if mine else ""))

    total = len(ref)
    print(f"objdump: {total} instrucoes")
    for cls in ("igual", "formato aceito", "objdump rejeita", "nos rejeitamos", "texto diferente",
                "palavra ausente/diferente"):
        print(f"  {cls:26s} {classes[cls]:9d}  ({100 * classes[cls] / max(total, 1):.4f}%)")
    extra = [a for a in set(ours) - set(ref) if ours[a][0] != "00000000"]
    print(f"  so no nosso lado (objdump omite blocos de zeros): {len(set(ours) - set(ref))}, nao-zero: {len(extra)}")
    for (cls, a, b), items in sorted(groups.items(), key=lambda kv: (kv[0][0], -len(kv[1]))):
        print(f"\n[{cls}] {len(items):7d}  objdump '{a}' -> nosso '{b}'")
        for addr, word, t1, t2 in items[:args.examples]:
            print(f"          {addr:08x} {word}   {t1:40s} | {t2}")
    bad = classes["nos rejeitamos"] + classes["texto diferente"] + classes["palavra ausente/diferente"] + len(extra)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
