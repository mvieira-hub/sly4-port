"""Research helper: beam-search the targets of explicit index values that are
smaller than the header 'base' (values >= base are taken as value - base),
maximising mesh consistency (unique directed edges, normals agreeing with the
triangle winding). Prints, for each small value, the chosen target and the
decoder state, to infer the encoding rule.

Usage: edge_solve.py FILE [--max-nv 64] [--beam 64] [--segments N]
"""
import argparse
import math
import struct

import edge_probe as E

PERM = ((2, 0), (1, 2), (0, 1))


def n3(v):
    def sg(x, b):
        x &= (1 << b) - 1
        return (x - (1 << b) if x >> (b - 1) else x) / ((1 << (b - 1)) - 1)
    return sg(v, 11), sg(v >> 11, 11), sg(v >> 22, 10)


def coherence(P, N, t):
    a, b, c = t
    ux, uy, uz = (P[b][i] - P[a][i] for i in range(3))
    vx, vy, vz = (P[c][i] - P[a][i] for i in range(3))
    n = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
    ln = math.sqrt(sum(x * x for x in n)) or 1e-12
    avg = [N[a][i] + N[b][i] + N[c][i] for i in range(3)]
    return sum(n[i] * avg[i] for i in range(3)) / ln / 3


def solve(seg, beam_width):
    nv = seg["nv"]
    P = [struct.unpack_from(">3f", seg["s0"], i * 12) for i in range(nv)]
    N = [n3(struct.unpack_from(">I", seg["s1"], i * 12)[0]) for i in range(nv)]
    idx = seg["idx"]
    a, base, nmask = struct.unpack_from(">HHH", idx, 0)
    width = idx[6]
    mask = E.Bits(idx[8:8 + nmask])
    st = E.Bits(idx[8 + nmask:])
    codes = [st.read(2) for _ in range(seg["ni"] // 3)]
    st.align()
    ex = [st.read(width) for _ in range(a)]
    # slot plan: list per triangle of (is_explicit, value)
    plan, ei = [], 0
    for k, code in enumerate(codes):
        n = 3 if code == 3 or k == 0 else 1
        slots = []
        for _ in range(n):
            if mask.read(1):
                slots.append(ex[ei])
                ei += 1
            else:
                slots.append(None)
        plan.append(slots)
    # beam state: (score, nxt, prev, edges, tris, small_choices)
    beam = [(0.0, 0, None, frozenset(), (), ())]
    for k, code in enumerate(codes):
        nbeam = []
        for score, nxt, prev, edges, tris, choices in beam:
            options = [((), nxt)]
            for v in plan[k]:
                nopts = []
                for vals, nx in options:
                    if v is None:
                        nopts.append((vals + (nx,), nx + 1))
                    elif v >= base:
                        nopts.append((vals + (v - base,), nx))
                    else:
                        for cand in range(nv):
                            nopts.append((vals + (("s", v, cand, nx),), nx))
                options = nopts
            for vals, nx in options:
                concrete = tuple(x[2] if isinstance(x, tuple) else x for x in vals)
                small = tuple(x for x in vals if isinstance(x, tuple))
                if code == 3 or prev is None:
                    tri = concrete
                else:
                    i, j = PERM[code]
                    tri = (prev[j], prev[i], concrete[0])
                if len(set(tri)) < 3 or max(tri) >= nv or min(tri) < 0:
                    continue
                es = ((tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0]))
                if any(e in edges for e in es):
                    continue
                c = coherence(P, N, tri)
                nbeam.append((score + c, nx, tri, edges | set(es), tris + (tri,), choices + small))
        nbeam.sort(key=lambda s: -s[0])
        beam = nbeam[:beam_width]
        if not beam:
            return None, k, base
    return beam[0], len(codes), base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--max-nv", type=int, default=64)
    ap.add_argument("--beam", type=int, default=64)
    ap.add_argument("--segments", type=int, default=12)
    args = ap.parse_args()
    segs = [s for s in E.segments(args.file)
            if s["cfg"][2] == 0 and s["cfg"][3] == 0xFF and len(s["s1"]) >= 12 * s["nv"] and s["nv"] <= args.max_nv
            and struct.unpack_from(">H", s["idx"], 2)[0] > 0]
    for s in segs[:args.segments]:
        best, reached, base = solve(s, args.beam)
        nt = s["ni"] // 3
        if best is None:
            print(f"nv={s['nv']} nt={nt} base={base}: sem solucao (parou no tri {reached})")
            continue
        avg = best[0] / nt
        print(f"nv={s['nv']} nt={nt} base={base} coerencia media={avg:.2f}  pequenos (valor, alvo, nxt):",
              [(v, cand, nx) for _s, v, cand, nx in best[5]])


if __name__ == "__main__":
    main()
