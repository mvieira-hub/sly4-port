"""Independent checks of the recompiler's control-flow analysis (sly4_recomp analyze).

  check_analysis.py --objdump objdump_raw.txt --analysis work/analysis [--eboot-scan work/eboot/functions.csv]

1. Words GNU objdump cannot decode (".long") must not be code, except mftb, which is
   real Cell code that binutils rejects with -M cell: those must all be code.
2. Unreached non-zero words: grouped in runs, with how many of them decode; long runs
   that decode almost entirely would be missed code.
3. Function entries versus eboot_scan.py (.opd + every "bl" found by a linear scan).
"""
import argparse
import bisect
import collections
import csv
import os
import re

LINE = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{2} [0-9a-f]{2} [0-9a-f]{2} [0-9a-f]{2})\s+(.*)$")


def load_wordmap(path):
    runs = []
    with open(path) as f:
        for r in csv.DictReader(f):
            runs.append((int(r["start"], 16), int(r["end"], 16), r["class"], int(r["words"]), int(r["nonzero"]),
                         int(r["decodable"])))
    return runs


def class_lookup(runs):
    starts = [r[0] for r in runs]

    def cls(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and runs[i][0] <= addr < runs[i][1]:
            return runs[i][2]
        return "unreached"  # zero runs are not listed
    return cls


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--analysis", required=True)
    ap.add_argument("--eboot-scan")
    args = ap.parse_args()
    runs = load_wordmap(os.path.join(args.analysis, "wordmap.csv"))
    cls = class_lookup(runs)

    rejected, mftb = collections.Counter(), collections.Counter()
    rejected_in_code = []
    with open(args.objdump, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            addr, word = int(m.group(1), 16), m.group(2).replace(" ", "")
            text = m.group(3).split()
            if not text or text[0] != ".long":
                continue
            # mftb rT,268/269: opcode 31, xo 371
            is_mftb = (int(word, 16) >> 26) == 31 and ((int(word, 16) >> 1) & 0x3FF) == 371
            c = cls(addr)
            (mftb if is_mftb else rejected)[c] += 1
            if c == "code" and not is_mftb:
                rejected_in_code.append((addr, word))
    print("1) palavras que o objdump rejeita (.long)")
    print(f"   mftb (codigo real):     {dict(mftb)}")
    print(f"   demais (dados/reserv.): {dict(rejected)}")
    for addr, word in rejected_in_code[:10]:
        print(f"   ATENCAO: {addr:08x} {word} rejeitada pelo objdump mas classificada como codigo")

    unreached = [r for r in runs if r[2] == "unreached"]
    total_nz = sum(r[4] for r in unreached)
    decodable = sum(r[5] for r in unreached)
    print(f"\n2) nao alcancadas: {len(unreached)} trechos, {total_nz} palavras nao nulas, {decodable} decodificaveis")
    suspicious = [r for r in unreached if r[4] >= 8 and r[5] >= 0.9 * r[3]]
    print(f"   trechos >= 8 palavras com >= 90% decodificavel (candidatos a codigo perdido): {len(suspicious)}, "
          f"{sum(r[4] for r in suspicious)} palavras")
    for r in sorted(unreached, key=lambda r: -r[4])[:12]:
        print(f"   {r[0]:08x}-{r[1]:08x} {r[3]:6d} palavras, {r[4]:6d} nao nulas, {r[5]:6d} decodificaveis")

    if args.eboot_scan:
        ours = {}
        with open(os.path.join(args.analysis, "functions.csv")) as f:
            for r in csv.DictReader(f):
                ours[int(r["entry"], 16)] = r
        theirs = set()
        with open(args.eboot_scan) as f:
            for r in csv.DictReader(f):
                theirs.add(int(r["address"], 16))
        only_theirs = sorted(theirs - set(ours))
        only_ours = sorted(set(ours) - theirs)
        print(f"\n3) entradas de funcao: nossas {len(ours)}, eboot_scan {len(theirs)}, em comum {len(theirs & set(ours))}")
        by_cls = collections.Counter(cls(a) for a in only_theirs)
        print(f"   so no eboot_scan: {len(only_theirs)} (onde caem na nossa analise: {dict(by_cls)})")
        for a in only_theirs[:6]:
            print(f"     {a:08x} ({cls(a)})")
        kinds = collections.Counter(ours[a]["kind"] for a in only_ours)
        print(f"   so na nossa analise: {len(only_ours)} {dict(kinds)}")
        for a in only_ours[:6]:
            print(f"     {a:08x} {ours[a]['kind']} {ours[a]['name']}")


if __name__ == "__main__":
    main()
