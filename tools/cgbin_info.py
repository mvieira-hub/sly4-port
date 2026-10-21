"""Inventory of the RSX Cg binaries (.vpo/.fpo) in Shaders.Zip.

Each file is a CgBinaryProgram (big-endian):
  u32 profile (7003 = sce_vp_rsx, 7004 = sce_fp_rsx), u32 binaryFormatRevision,
  u32 totalSize, u32 parameterCount, u32 parameterArray, u32 program,
  u32 ucodeSize, u32 ucode
  CgBinaryParameter (48 bytes): type, res, var, resIndex, name, defaultValue,
  embeddedConst, semantic, direction, paramno, isReferenced, isShared
  CgBinaryVertexProgram:   instructionCount, instructionSlot, registerCount,
                           attributeInputMask, attributeOutputMask, userClipMask
  CgBinaryFragmentProgram: instructionCount, attributeInputMask, partialTexType,
                           u16 texCoordsInputMask, texCoords2D, texCoordsCentroid,
                           u8 registerCount, outputFromH0, depthReplace, pixelKill
Writes summary.csv, one JSON per shader and the raw ucode (.ucode) to --out.
Usage:
  cgbin_info.py [--zip /work/psarc/System/Shaders.Zip] [--out /work/shaders]
"""
import argparse
import collections
import csv
import json
import os
import struct
import sys
import zipfile

PROFILES = {7003: "sce_vp_rsx", 7004: "sce_fp_rsx"}
# Cg runtime enum values (CGtype / CGenum)
TYPES = {1045: "float", 1046: "float2", 1047: "float3", 1048: "float4", 1064: "float4x4",
         1065: "sampler1D", 1066: "sampler2D", 1067: "sampler3D", 1068: "samplerRECT", 1069: "samplerCUBE",
         1091: "half", 1092: "half2", 1093: "half3", 1094: "half4", 1114: "bool"}
VARS = {4101: "varying", 4102: "uniform", 4103: "constant"}
DIRS = {4097: "in", 4098: "out", 4099: "inout"}


def cstr(b, o):
    return b[o:b.index(b"\0", o)].decode("ascii", "replace") if o else ""


def parse(b):
    profile, rev, total, nparam, parr, prog, usize, uoff = struct.unpack_from(">8I", b, 0)
    params = []
    for i in range(nparam):
        (typ, res, var, resi, name, dflt, emb, sem, dirn, pno, ref, shared) = struct.unpack_from(">IIIiIIIIIiII", b, parr + 48 * i)
        p = {"name": cstr(b, name), "semantic": cstr(b, sem), "type": TYPES.get(typ, typ),
             "var": VARS.get(var, var), "direction": DIRS.get(dirn, dirn), "resource": res,
             "resIndex": resi, "referenced": bool(ref)}
        if dflt:
            p["default"] = list(struct.unpack_from(">4f", b, dflt))
        if emb:
            count = struct.unpack_from(">I", b, emb)[0]
            p["embeddedConstOffsets"] = list(struct.unpack_from(f">{count}I", b, emb + 4))
        params.append(p)
    info = {"profile": PROFILES.get(profile, profile), "revision": rev, "totalSize": total,
            "ucodeSize": usize, "params": params}
    if profile == 7003:
        keys = ("instructionCount", "instructionSlot", "registerCount", "attributeInputMask",
                "attributeOutputMask", "userClipMask")
        info["program"] = dict(zip(keys, struct.unpack_from(">6I", b, prog)))
    elif profile == 7004:
        ic, aim, ptt, tcim, tc2d, tccen, rc, h0, dr, pk = struct.unpack_from(">IIIHHHBBBB", b, prog)
        info["program"] = {"instructionCount": ic, "attributeInputMask": aim, "partialTexType": ptt,
                           "texCoordsInputMask": tcim, "texCoords2D": tc2d, "texCoordsCentroid": tccen,
                           "registerCount": rc, "outputFromH0": h0, "depthReplace": dr, "pixelKill": pk}
    return info, b[uoff:uoff + usize]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zip", default="/work/psarc/System/Shaders.Zip")
    ap.add_argument("--out", default="/work/shaders")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    rows, stats = [], collections.Counter()
    with zipfile.ZipFile(args.zip) as z:
        for n in sorted(z.namelist()):
            if not n.lower().endswith((".vpo", ".fpo")):
                continue
            info, ucode = parse(z.read(n))
            base = os.path.join(args.out, n)
            with open(base + ".json", "w") as f:
                json.dump(info, f, indent=1)
            with open(base + ".ucode", "wb") as f:
                f.write(ucode)
            used = [p for p in info["params"] if p["referenced"]]
            samplers = [p for p in used if str(p["type"]).startswith("sampler")]
            uniforms = [p for p in used if p["var"] == "uniform" and p not in samplers]
            prog = info.get("program", {})
            stats[info["profile"]] += 1
            stats["instructions_" + str(info["profile"])] += prog.get("instructionCount", 0)
            if prog.get("pixelKill"):
                stats["fp_with_kill"] += 1
            if prog.get("depthReplace"):
                stats["fp_with_depth"] += 1
            rows.append([n, info["profile"], prog.get("instructionCount", ""), prog.get("registerCount", ""),
                         len(used), len(uniforms), len(samplers),
                         " ".join(p["name"] for p in samplers), " ".join(p["name"] for p in uniforms)])

    with open(os.path.join(args.out, "summary.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["file", "profile", "instructions", "registers", "params_used", "uniforms", "samplers",
                    "sampler_names", "uniform_names"])
        w.writerows(rows)
    for k, v in sorted(stats.items()):
        print(f"{k:<28} {v}", file=sys.stderr)
    print(f"{len(rows)} shaders -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
