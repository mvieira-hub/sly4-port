"""Research helper: ground-truth samples for EDGE explicit index values.

Restart triangles (code 3) do not depend on the previous triangle, and the
new-vertex counter only depends on the mask, so for every restart triangle
with one explicit slot the explicit target can be found from geometry alone:
the vertex that makes the most compact triangle whose winding agrees with the
vertex normals. Prints (value, target, nxt, base, width, slot index) samples
and tests candidate formulas.
Usage: edge_samples.py FILE
"""
import argparse
import collections
import math
import struct

import edge_probe as E
import edge_solve as S


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    args = ap.parse_args()
    samples = []
    for s in E.segments(args.file):
        nv = s["nv"]
        if s["cfg"][2] != 0 or s["cfg"][3] != 0xFF or len(s["s1"]) < 12 * nv:
            continue
        P = [struct.unpack_from(">3f", s["s0"], i * 12) for i in range(nv)]
        N = [S.n3(struct.unpack_from(">I", s["s1"], i * 12)[0]) for i in range(nv)]
        idx = s["idx"]
        a, base, nm = struct.unpack_from(">HHH", idx, 0)
        width = idx[6]
        mask = E.Bits(idx[8:8 + nm])
        st = E.Bits(idx[8 + nm:])
        codes = [st.read(2) for _ in range(s["ni"] // 3)]
        st.align()
        ex = [st.read(width) for _ in range(a)]
        ei, nxt, slot_no = 0, 0, 0
        for k, code in enumerate(codes):
            n = 3 if code == 3 or k == 0 else 1
            slots = []
            for _ in range(n):
                if mask.read(1):
                    slots.append(("x", ex[ei], ei))
                    ei += 1
                else:
                    slots.append(("n", nxt))
                    nxt += 1
                slot_no += 1
            if n == 3 and sum(1 for x in slots if x[0] == "x") == 1:
                best = None
                for cand in range(nv):
                    t = tuple(cand if x[0] == "x" else x[1] for x in slots)
                    if len(set(t)) < 3:
                        continue
                    c = S.coherence(P, N, t)
                    per = sum(math.dist(P[t[i]], P[t[(i + 1) % 3]]) for i in range(3))
                    key = (c > 0.5, -per)
                    if best is None or key > best[0]:
                        best = (key, cand)
                xs = next(x for x in slots if x[0] == "x")
                samples.append({"v": xs[1], "t": best[1], "nxt": nxt, "base": base, "w": width,
                                "ei": xs[2], "a": a, "nv": nv})
    print(len(samples), "amostras")
    forms = {
        "t = v": lambda s: s["v"],
        "t = v - base": lambda s: s["v"] - s["base"],
        "t = nxt-1-v": lambda s: s["nxt"] - 1 - s["v"],
        "t = nxt-v": lambda s: s["nxt"] - s["v"],
        "t = nxt-1-(v-base)": lambda s: s["nxt"] - 1 - (s["v"] - s["base"]),
        "t = nxt-(v-base)": lambda s: s["nxt"] - (s["v"] - s["base"]),
        "t = nxt-1-v+base": lambda s: s["nxt"] - 1 - s["v"] + s["base"],
        "t = v+base": lambda s: s["v"] + s["base"],
    }
    for name, f in forms.items():
        print(f"{name:<22} {sum(1 for s in samples if f(s) == s['t'])}/{len(samples)}")
    diffs = collections.Counter(s["nxt"] - s["t"] for s in samples)
    print("nxt - alvo (mais comuns):", diffs.most_common(12))
    for s in samples[:30]:
        print(s, " nxt-t=", s["nxt"] - s["t"], " v-t=", s["v"] - s["t"])


if __name__ == "__main__":
    main()
