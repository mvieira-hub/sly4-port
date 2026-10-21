"""Survey of every Havok animation (HKAF/HKAD) in .san files: animation classes,
rotation/position quantization in use, and clips that havok.py cannot decode.

  havok_survey.py /work/psarc/Levels
"""
import hashlib
import struct
import sys
from collections import Counter

import havok
import sanchunks
from san2gltf import collect_inputs, cstr

ROT_NAMES = {0: "POLAR32", 1: "THREECOMP40", 2: "THREECOMP48", 3: "THREECOMP24", 4: "STRAIGHT16", 5: "UNCOMPRESSED"}


def main():
    classes, rot, quant, errors = Counter(), Counter(), Counter(), Counter()
    seen, failed = set(), {}
    files = clips = 0
    for path in collect_inputs(sys.argv[1:]):
        with open(path, "rb") as f:
            buf = f.read()
        files += 1
        for c in sanchunks.walk(buf, max_depth=2):
            if c.tag != "HKAF" or c.path.count("/") != 3:
                continue
            parts = {k.tag: buf[k.data_start:k.end] for k in sanchunks.children(buf, c)}
            if "HKAD" not in parts:
                continue
            clips += 1
            key = hashlib.sha1(parts["HKAD"]).digest()
            if key in seen:
                continue
            seen.add(key)
            name = cstr(parts.get("HKAH", b"\0" * 65)[1:65])
            try:
                pf = havok.Packfile(parts["HKAD"])
                binding = havok.read_binding(pf, pf.find("hkaAnimationBinding")[0])
                o = binding["animation"]
                cls = next(cc for oo, cc in pf.objects if oo == o)
                classes[cls] += 1
                if cls == "hkaSplineCompressedAnimation":
                    # masks of every block: quantization byte, then pos/rot/scale masks
                    n, bp, bn = pf.s32(o + 0x10), *pf.array(o + 0x44)
                    dp = pf.array(o + 0x74)[0]
                    for b in range(bn):
                        start = dp + pf.u32(bp + 4 * b)
                        for t in range(n):
                            q, pm, rm, sm = pf.d[start + 4 * t:start + 4 * t + 4]
                            if rm:
                                rot[ROT_NAMES.get((q >> 2) & 15, (q >> 2) & 15)] += 1
                            if pm & 0xF0:
                                quant[f"posicao {8 if q & 3 == 0 else 16} bits"] += 1
                havok.read_animation(pf, o)
            except Exception as e:  # report every failure, whatever its kind
                label = f"{type(e).__name__}: {e}"
                errors[label] += 1
                failed.setdefault(label, (path, name))
    print(f"{files} arquivos, {clips} clipes ({len(seen)} distintos)")
    for title, counter in (("classes", classes), ("rotacoes (trilhas x blocos)", rot),
                           ("posicao em spline", quant), ("falhas", errors)):
        print(f"\n{title}:")
        for k, v in counter.most_common():
            print(f"  {v:8d}  {k}")
    for label, (path, name) in failed.items():
        print(f"  exemplo de '{label}': {name} em {path}")


if __name__ == "__main__":
    main()
