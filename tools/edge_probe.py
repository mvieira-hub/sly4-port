"""Research helper: collects EDGE (MEDG) segments and tests hypotheses about the
compressed index bit-stream layout. Not part of the export pipeline.

Usage: edge_probe.py FILE [--dump N]
"""
import argparse
import struct
import sys

import sanchunks


def align16(x):
    return (x + 15) & ~15


def segments(path):
    """Yields dicts for every EDGE segment (MEDG) in a .san file."""
    with open(path, "rb") as f:
        buf = f.read()
    for c in sanchunks.walk(buf, max_depth=6):
        if c.tag != "MEDG":
            continue
        e = buf[c.data_start:c.end]
        n = struct.unpack_from("<I", e, 0)[0]
        o = 4
        for _ in range(n):
            sz = struct.unpack_from("<14H", e, o)
            rsx, x3c, x40 = struct.unpack_from("<3I", e, o + 28)
            cfg = e[o + 40:o + 56]
            blob = o + 56
            nv, ni = struct.unpack_from(">HH", cfg, 8)
            idx = e[blob:blob + sz[0] + sz[1]]
            p = blob + align16(sz[0] + sz[1])
            s0 = e[p:p + sz[2] + sz[3] + sz[4]]
            p += align16(sz[2] + sz[3] + sz[4])
            s1 = e[p:p + sz[5] + sz[6] + sz[7]]
            p += align16(sz[5] + sz[6] + sz[7])
            skin = e[p:p + sz[12] + sz[13]]
            p += sz[12] + sz[13]
            rsx_data = e[p:p + rsx]
            o = p + rsx
            yield {"nv": nv, "ni": ni, "cfg": cfg, "sizes": sz, "idx": idx, "s0": s0, "s1": s1,
                   "skin": skin, "rsx": rsx_data, "x3c": x3c, "x40": x40}


def bits_of(b):
    return "".join(f"{x:08b}" for x in b)


class Bits:
    def __init__(self, data, pos=0, lsb=False):
        self.s, self.p = ("".join(f"{x:08b}"[::-1] for x in data) if lsb else bits_of(data)), pos

    def read(self, n):
        v = int(self.s[self.p:self.p + n] or "0", 2) if n else 0
        self.p += n
        return v

    def align(self):
        self.p = (self.p + 7) & ~7


EDGE_PERMS = [(0, 1), (1, 2), (2, 0)]


def decode_indexes(idx, ni, code_edges, tri_order, explicit_mode, restart=3, lsb=(False, False, False)):
    """Candidate decoder. code_edges: edge (pair of corner positions) for codes 0,1,2;
    tri_order: how the new triangle is built from edge (e0, e1) and new vertex x;
    lsb: bit order (mask, codes, explicit)."""
    a, base, nmask = struct.unpack_from(">HHH", idx, 0)
    width = idx[6]
    mask = Bits(idx[8:8 + nmask], lsb=lsb[0])
    nt = ni // 3
    stream = Bits(idx[8 + nmask:], lsb=lsb[1])
    codes = [stream.read(2) for _ in range(nt)]
    stream.align()
    ex_stream = Bits(idx[8 + nmask:], stream.p, lsb=lsb[2])
    explicit = [ex_stream.read(width) for _ in range(a)]
    if explicit_mode == "delta8":
        # SPU decompressor 0xD5F4B0: index[i] = (value[i] - base) + index[i - 8]
        for i in range(len(explicit)):
            explicit[i] = (explicit[i] - base + (explicit[i - 8] if i >= 8 else 0)) & 0xFFFF
    ei, nxt, out, prev = 0, 0, [], None
    if explicit_mode == "newbase":
        nxt = base  # indices below base are only reachable through explicit values
    fifo = []
    used = set()
    if explicit_mode.startswith("split"):
        # how many explicit slots belong to restart triangles (to locate the second list)
        probe, n_restart, first = Bits(idx[8:8 + nmask]), 0, True
        for code in codes:
            if code == restart or first:
                n_restart += sum(probe.read(1) for _ in range(3))
            else:
                probe.read(1)
            first = False
        lists = {"restart": explicit[:n_restart], "edge": explicit[n_restart:]}
        if explicit_mode == "split_edge_first":
            n_edge = len(explicit) - n_restart
            lists = {"edge": explicit[:n_edge], "restart": explicit[n_edge:]}
        ptr = {"restart": 0, "edge": 0}
    kind = "restart"
    # explicit_mode "fifo:<push>:<end>:<window>" push in slots|tris|tris_nodup|slots_nodup,
    # end oldest|newest, window win|inf
    fm = explicit_mode.split(":") if explicit_mode.startswith("fifo") else None
    win = (1 << width) if fm and fm[3] == "win" else 1 << 30

    def push(v):
        if fm[1].endswith("nodup") and v in fifo:
            fifo.remove(v)
        fifo.append(v)
        while len(fifo) > win:
            fifo.pop(0)

    def slot():
        nonlocal ei, nxt
        if mask.read(1):
            if explicit_mode.startswith("split"):
                lst = lists[kind]
                v = lst[ptr[kind]] if ptr[kind] < len(lst) else -1
                ptr[kind] += 1
            else:
                v = explicit[ei] if ei < len(explicit) else -1
            ei += 1
            if explicit_mode.startswith("split"):
                r = v
            elif fm:
                if not 0 <= v < len(fifo):
                    r = -1
                else:
                    r = fifo[v] if fm[2] == "oldest" else fifo[len(fifo) - 1 - v]
            elif explicit_mode in ("abs", "abs_skip", "newbase", "delta8"):
                r = v
            elif explicit_mode == "minus_base":
                r = v - base
            elif explicit_mode.startswith("two:"):
                # values >= base are absolute (+base); smaller values are short back-references
                if v >= base:
                    r = v - base
                else:
                    r = {"two:nxt-1-v": nxt - 1 - v, "two:nxt-v": nxt - v, "two:nxt-2-v": nxt - 2 - v,
                         "two:nxt+v": nxt + v, "two:nxt+1+v": nxt + 1 + v}[explicit_mode]
            elif explicit_mode == "plus_base":
                r = v + base
            elif explicit_mode == "slide":
                r = v + max(0, nxt - (1 << width))
            elif explicit_mode == "slide1":
                r = v + max(0, nxt - 1 - (1 << width))
            elif explicit_mode == "back":
                r = nxt - 1 - v
            else:
                r = nxt - v  # "back0"
        else:
            if explicit_mode == "abs_skip":
                while nxt in used:
                    nxt += 1
            nxt += 1
            r = nxt - 1
        used.add(r)
        if fm and fm[1].startswith("slots"):
            push(r)
        return r

    for code in codes:
        if code == restart or prev is None:
            kind = "restart"
            tri = (slot(), slot(), slot())
        else:
            kind = "edge"
            i, j = code_edges[code]
            e0, e1 = prev[i], prev[j]
            x = slot()
            tri = {"rev": (e1, e0, x), "fwd": (e0, e1, x), "xrev": (x, e1, e0)}[tri_order]
        if fm and fm[1].startswith("tris"):
            for v in tri:
                push(v)
        out.append(tri)
        prev = tri
    return out


def score(tris, nv):
    """Fraction of a decoded triangle list that looks like a valid oriented mesh."""
    if any(v < 0 or v >= nv for t in tris for v in t):
        return 0.0
    if len({v for t in tris for v in t}) != nv:
        return 0.1
    seen, bad = set(), 0
    for a, b, c in tris:
        if len({a, b, c}) < 3:
            bad += 1
        for e in ((a, b), (b, c), (c, a)):
            if e in seen:
                bad += 1
            seen.add(e)
    return 1.0 - bad / (3 * len(tris))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--dump", type=int, default=0)
    ap.add_argument("--decode", action="store_true", help="search the index decoder variants")
    args = ap.parse_args()
    segs = list(segments(args.file))
    if args.decode:
        import itertools
        small = [s for s in segs if s["nv"] <= 200]
        results = []
        modes = ["abs", "minus_base", "two:nxt+v", "two:nxt+1+v"]
        perm = ((2, 0), (1, 2), (0, 1))
        for mode in modes:
            sc = [score(decode_indexes(s["idx"], s["ni"], perm, "rev", mode), s["nv"]) for s in segs]
            results.append((sum(x == 1.0 for x in sc), round(sum(sc) / len(sc), 4), perm, "rev", mode))
        print(f"{len(small)} segmentos pequenos")
        results.sort(reverse=True)
        for r in results[:10]:
            print(r)
        return
    rows = []
    for s in segs:
        idx = s["idx"]
        a, z, b = struct.unpack_from(">HHH", idx, 0)
        c, d = idx[6], idx[7]
        bits = bits_of(idx[8:])
        last = bits.rfind("1")
        rows.append((s["nv"], s["ni"] // 3, a, b, c, last, len(bits)))
    # candidate total-length formulas: (name, f(nv, nt, A, B, C))
    forms = {
        "2nt+2nt+A*B": lambda nv, nt, a, b, c: 4 * nt + a * b,
        "2nt+2nt+A*C": lambda nv, nt, a, b, c: 4 * nt + a * c,
        "2nt+A*B": lambda nv, nt, a, b, c: 2 * nt + a * b,
        "2nt+A*C": lambda nv, nt, a, b, c: 2 * nt + a * c,
        "nt+2nt+A*B": lambda nv, nt, a, b, c: 3 * nt + a * b,
        "nt+2nt+A*C": lambda nv, nt, a, b, c: 3 * nt + a * c,
        "2nt+A*B+C*nv": lambda nv, nt, a, b, c: 2 * nt + a * b,
        "2nt+nv+A*B": lambda nv, nt, a, b, c: 2 * nt + nv + a * b,
        "2nt+nv+A*C": lambda nv, nt, a, b, c: 2 * nt + nv + a * c,
        "3nt+A*B+C": lambda nv, nt, a, b, c: 3 * nt + a * b + c,
        "2nt+A*B+nt*C?": lambda nv, nt, a, b, c: 2 * nt + a * b,
        "8+2nt+A*B": lambda nv, nt, a, b, c: 8 + 2 * nt + a * b,
        "8+2nt+A*C": lambda nv, nt, a, b, c: 8 + 2 * nt + a * c,
        "A*B+2nt+(nt)*1": lambda nv, nt, a, b, c: a * b + 3 * nt,
    }
    for name, f in forms.items():
        ok = sum(1 for nv, nt, a, b, c, last, n in rows if last < f(nv, nt, a, b, c) <= n and n - f(nv, nt, a, b, c) < 128)
        print(f"{name:<22} {ok}/{len(rows)}")
    # integer search: total = k1*nt + k2*nv + A*C + k3*B + k4 must satisfy used <= total <= avail
    best = []
    for k1 in range(0, 4):
        for k2 in range(0, 3):
            for k3 in range(0, 17):
                for k4 in range(0, 17):
                    ok = sum(1 for nv, nt, a, b, c, last, n in rows if last < k1 * nt + k2 * nv + a * c + k3 * b + k4 <= n)
                    best.append((ok, k1, k2, k3, k4))
    best.sort(reverse=True)
    print("melhores (ok, k1*nt, k2*nv, k3*B, k4):", best[:8])
    for r in rows[:args.dump]:
        print(r)


if __name__ == "__main__":
    main()
