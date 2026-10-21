"""Extracts TEXR textures from .san / .san.cooked files to DDS.

TEXR chunk = TXRH + TGTF. TXRH (little-endian; field names from the game's
reader at EBOOT 0x0051D4B4): u8 ver, u32 nameHash, u16 width, u16 height,
u8 depth, u8 bpp, u8 type, char name[32], u32 packBufOfst, u32 packBufSize.
TGTF is a Sony GTF file (big-endian):
  CellGtfFileHeader      { u32 version (0x020101FF or 0x01040100); u32 size; u32 numTexture }
  CellGtfTextureAttribute{ u32 id; u32 offsetToTex; u32 textureSize; CellGcmTexture }
  CellGcmTexture         { u8 format, mipmap, dimension, cubemap; u32 remap;
                           u16 width, height, depth; u8 location, pad; u32 pitch, offset }
format bit 0x20 = LN (linear; otherwise swizzled), 0x40 = UN (unnormalized).
DXT data is stored in blocks like DDS; uncompressed swizzled data is in Morton
(Z-order) per mip level and big-endian per texel, so it is unswizzled and
byte-swapped to the DDS little-endian layout.

Usage:
  texr2dds.py INPUT... [--out DIR] [--png] [--info]
INPUT may be files or directories (searched for *.san and *.cooked).
"""
import argparse
import collections
import io
import os
import re
import struct
import sys

import sanchunks

# Both GTF tool revisions seen in the game share the same header layout.
GTF_VERSIONS = {0x020101FF, 0x01040100}
FLAG_LN, FLAG_UN = 0x20, 0x40

# base format -> (name, bytes per texel or DXT block, is_dxt)
FORMATS = {
    0x81: ("B8", 1, False),
    0x82: ("A1R5G5B5", 2, False),
    0x83: ("A4R4G4B4", 2, False),
    0x84: ("R5G6B5", 2, False),
    0x85: ("A8R8G8B8", 4, False),
    0x86: ("DXT1", 8, True),
    0x87: ("DXT3", 16, True),
    0x88: ("DXT5", 16, True),
    0x8B: ("G8B8", 2, False),
}

# DDS pixel formats: (flags, fourcc, bitcount, rmask, gmask, bmask, amask)
DDPF_ALPHAPIXELS, DDPF_FOURCC, DDPF_RGB, DDPF_LUMINANCE = 0x1, 0x4, 0x40, 0x20000
DDS_PF = {
    "DXT1": (DDPF_FOURCC, b"DXT1", 0, 0, 0, 0, 0),
    "DXT3": (DDPF_FOURCC, b"DXT3", 0, 0, 0, 0, 0),
    "DXT5": (DDPF_FOURCC, b"DXT5", 0, 0, 0, 0, 0),
    "A8R8G8B8": (DDPF_RGB | DDPF_ALPHAPIXELS, b"\0" * 4, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000),
    "A1R5G5B5": (DDPF_RGB | DDPF_ALPHAPIXELS, b"\0" * 4, 16, 0x7C00, 0x3E0, 0x1F, 0x8000),
    "A4R4G4B4": (DDPF_RGB | DDPF_ALPHAPIXELS, b"\0" * 4, 16, 0xF00, 0xF0, 0xF, 0xF000),
    "R5G6B5": (DDPF_RGB, b"\0" * 4, 16, 0xF800, 0x7E0, 0x1F, 0),
    "B8": (DDPF_LUMINANCE, b"\0" * 4, 8, 0xFF, 0, 0, 0),
    # G8B8 has two channels; stored as A8L8 (high byte = alpha) to stay readable.
    "G8B8": (DDPF_LUMINANCE | DDPF_ALPHAPIXELS, b"\0" * 4, 16, 0xFF, 0, 0, 0xFF00),
}


class Texture:
    def __init__(self, name, src, fmt_raw, mips, dim, cube, remap, w, h, d, pitch, data):
        self.name, self.src, self.txrh = name, src, {}
        self.fmt_raw, self.mips, self.dim, self.cube, self.remap = fmt_raw, mips, dim, cube, remap
        self.w, self.h, self.d, self.pitch, self.data = w, h, d, pitch, data

    @property
    def base(self):
        return self.fmt_raw & ~(FLAG_LN | FLAG_UN)

    @property
    def linear(self):
        return bool(self.fmt_raw & FLAG_LN)

    @property
    def fmt_name(self):
        return FORMATS.get(self.base, (f"0x{self.base:02X}",))[0]


def parse_txrh(payload):
    if len(payload) < 12:
        return {}
    ver, name_hash, w, h, depth, bpp, typ = struct.unpack_from("<BIHHBBB", payload, 0)
    name = payload[12:44].split(b"\0", 1)[0].decode("ascii", "replace")
    return {"ver": ver, "nameHash": name_hash, "width": w, "height": h, "depth": depth,
            "bpp": bpp, "type": typ, "name": name or None}


def parse_gtf(buf, src, name):
    version, _size, count = struct.unpack_from(">III", buf, 0)
    if version not in GTF_VERSIONS:
        raise ValueError(f"GTF version 0x{version:08X}")
    out = []
    for i in range(count):
        a = 12 + i * 36
        _id, off, tsize = struct.unpack_from(">III", buf, a)
        fmt, mips, dim, cube, remap, w, h, d, _loc, _pad, pitch, _goff = struct.unpack_from(">BBBBIHHHBBII", buf, a + 12)
        label = name if count == 1 else f"{name}_{i}"
        out.append(Texture(label, src, fmt, max(mips, 1), dim, cube, remap, w, h, max(d, 1), pitch, buf[off:off + tsize]))
    return out


def iter_textures(path):
    with open(path, "rb") as f:
        buf = f.read()
    for c in sanchunks.walk(buf):
        if c.tag != "TEXR":
            continue
        hdr, gtf = {}, None
        for k in sanchunks.children(buf, c):
            if k.tag == "TXRH":
                hdr = parse_txrh(k.payload(buf))
            elif k.tag == "TGTF":
                gtf = k.payload(buf)
        if gtf is None:
            continue
        for t in parse_gtf(gtf, path, hdr.get("name") or f"tex_{c.start:08X}"):
            t.txrh = hdr
            yield t


# ---------- conversion ----------

def morton_table(w, h):
    """Linear index -> swizzled index for a power-of-two w x h level (x takes the low bit)."""
    table = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            idx, bit, xx, yy, bw, bh = 0, 0, x, y, w - 1, h - 1
            while bw or bh:
                if bw:
                    idx |= (xx & 1) << bit
                    bit += 1
                    xx >>= 1
                    bw >>= 1
                if bh:
                    idx |= (yy & 1) << bit
                    bit += 1
                    yy >>= 1
                    bh >>= 1
            table[y * w + x] = idx
    return table


_morton_cache = {}


def level_size(t, w, h):
    bpe, dxt = FORMATS[t.base][1], FORMATS[t.base][2]
    if dxt:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bpe
    return w * h * bpe


def swap_texels(data, bpe):
    if bpe == 1:
        return bytes(data)
    out = bytearray(data)
    for k in range(bpe):
        out[k::bpe] = data[bpe - 1 - k::bpe]
    return bytes(out)


def convert_level(t, src, w, h):
    """Returns DDS-layout bytes for one mip level (src starts at the level)."""
    bpe, dxt = FORMATS[t.base][1], FORMATS[t.base][2]
    if dxt:
        return src[:level_size(t, w, h)]
    if t.linear:
        pitch = t.pitch or w * bpe
        rows = b"".join(src[y * pitch:y * pitch + w * bpe] for y in range(h))
    else:
        key = (w, h)
        if key not in _morton_cache:
            _morton_cache[key] = morton_table(w, h)
        table = _morton_cache[key]
        rows = bytearray(w * h * bpe)
        for i, s in enumerate(table):
            rows[i * bpe:(i + 1) * bpe] = src[s * bpe:(s + 1) * bpe]
    return swap_texels(rows, bpe)


def level_stride(t, w, h):
    """Bytes the level occupies in the GTF data."""
    if t.linear and not FORMATS[t.base][2]:
        return (t.pitch or w * FORMATS[t.base][1]) * h
    return level_size(t, w, h)


def to_dds_payload(t):
    faces = 6 if t.cube else 1
    face_bytes = 0
    w, h = t.w, t.h
    for _ in range(t.mips):
        face_bytes += level_stride(t, w, h)
        w, h = max(1, w // 2), max(1, h // 2)
    face_stride = face_bytes
    aligned = (face_bytes + 127) & ~127
    if faces > 1 and len(t.data) >= aligned * faces:
        face_stride = aligned
    out = bytearray()
    for f in range(faces):
        pos = f * face_stride
        w, h = t.w, t.h
        for _ in range(t.mips):
            out += convert_level(t, t.data[pos:], w, h)
            pos += level_stride(t, w, h)
            w, h = max(1, w // 2), max(1, h // 2)
    return bytes(out)


def dds_header(t):
    pf = DDS_PF[t.fmt_name]
    dxt = FORMATS[t.base][2]
    flags = 0x1 | 0x2 | 0x4 | 0x1000  # CAPS | HEIGHT | WIDTH | PIXELFORMAT
    flags |= 0x80000 if dxt else 0x8  # LINEARSIZE | PITCH
    pitch_or_linear = level_size(t, t.w, t.h) if dxt else t.w * FORMATS[t.base][1]
    caps, caps2 = 0x1000, 0  # TEXTURE
    if t.mips > 1:
        flags |= 0x20000
        caps |= 0x400008  # COMPLEX | MIPMAP
    if t.cube:
        caps |= 0x8
        caps2 = 0x200 | 0xFC00  # CUBEMAP + all faces
    hdr = struct.pack("<4sIIIIIII44x", b"DDS ", 124, flags, t.h, t.w, pitch_or_linear, 0, t.mips)
    hdr += struct.pack("<II4sIIIII", 32, pf[0], pf[1], pf[2], pf[3], pf[4], pf[5], pf[6])
    hdr += struct.pack("<IIIII", caps, caps2, 0, 0, 0)
    return hdr


def is_pow2(n):
    return n > 0 and n & (n - 1) == 0


def safe_name(name):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("._") or "tex"


def collect_inputs(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                for f in sorted(files):
                    if f.lower().endswith((".san", ".cooked")):
                        yield os.path.join(root, f)
        else:
            yield p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--out", default="/work/textures")
    ap.add_argument("--png", action="store_true", help="also write a PNG preview (Pillow)")
    ap.add_argument("--info", action="store_true", help="only list textures and formats")
    args = ap.parse_args()

    stats = collections.Counter()
    skipped = collections.Counter()
    written = 0
    for path in collect_inputs(args.inputs):
        rel = os.path.splitext(os.path.basename(path))[0].replace(".san", "")
        outdir = os.path.join(args.out, safe_name(os.path.basename(os.path.dirname(path))) + "_" + safe_name(rel))
        used = collections.Counter()
        for t in iter_textures(path):
            kind = f"{t.fmt_name}{'/LN' if t.linear else ''}{'/cube' if t.cube else ''}{'/3D' if t.dim == 3 else ''}"
            stats[kind] += 1
            if args.info:
                x = t.txrh
                print(f"{kind:<16} {t.w:>5}x{t.h:<5} mips={t.mips:<2} remap=0x{t.remap:04X} {len(t.data):>9}  "
                      f"bpp={x.get('bpp', '?')} type={x.get('type', '?')}  {t.name}  [{os.path.basename(path)}]")
                continue
            reason = None
            if t.base not in FORMATS:
                reason = f"formato {t.fmt_name}"
            elif t.dim == 3:
                reason = "textura 3D"
            elif not t.linear and not FORMATS[t.base][2] and not (is_pow2(t.w) and is_pow2(t.h)):
                reason = "swizzle com tamanho nao potencia de 2"
            base = safe_name(os.path.splitext(t.name)[0])
            used[base] += 1
            if used[base] > 1:
                base = f"{base}_{used[base]}"
            os.makedirs(outdir, exist_ok=True)
            if reason:
                skipped[reason] += 1
                with open(os.path.join(outdir, base + ".gtf_raw"), "wb") as f:
                    f.write(t.data)
                continue
            dds = dds_header(t) + to_dds_payload(t)
            dest = os.path.join(outdir, base + ".dds")
            with open(dest, "wb") as f:
                f.write(dds)
            written += 1
            if args.png:
                try:
                    from PIL import Image
                    with Image.open(io.BytesIO(dds)) as im:
                        im.save(os.path.join(outdir, base + ".png"))
                except Exception as e:  # preview is best-effort
                    skipped[f"png: {type(e).__name__}"] += 1

    print("\nFormatos encontrados:", file=sys.stderr)
    for k, v in stats.most_common():
        print(f"  {k:<18} {v}", file=sys.stderr)
    if not args.info:
        print(f"DDS gravados: {written} em {args.out}", file=sys.stderr)
        for k, v in skipped.items():
            print(f"  ignorado ({k}): {v}  -> salvo como .gtf_raw" if not k.startswith("png") else f"  {k}: {v}", file=sys.stderr)


if __name__ == "__main__":
    main()
