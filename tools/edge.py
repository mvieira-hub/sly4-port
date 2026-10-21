"""Decoder for the PlayStation EDGE geometry stored in MEDG chunks (skinned meshes).

MEDG (handler 0x0054A490): u32 numSegments (LE), then per segment
  u16 x14 (LE): indexesSizes[2], spuVertexesSizes[6], skinMatricesByteOffsets[2],
                skinMatricesSizes[2], skinIndexesAndWeightsSizes[2]
  u32 rsxOnlyVertexesSize, u32, u32
  16-byte EdgeGeomSpuConfigInfo (BE): flags, cmdHole, inputFmt, inputFmt2, outputFmt,
     deltaFmt, indexesFlavor<<4 | skinningFlavor, skinMatrixFmt, u16 numVertexes,
     u16 numIndexes, u32 indexesOffset
  blob: align16(indexes) + align16(stream 0) + align16(stream 1) + skin + rsx-only stream

Streams (validated against geometry and the skeleton):
  stream 0, inputFmt 0: position f32 x3 (BE)
  stream 0, inputFmt 7: position X11Y11Z10 signed-normalized (x bits 0-10), times MHDR.vtxScale
  stream 1: normal [, tangent, binormal] X11Y11Z10 signed-normalized u32 each
  skin: per vertex 4 x (u8 weight, u8 local matrix index); weights sum to 255
        local index -> global bone through skinMatricesByteOffsets/Sizes (48-byte matrices)
  rsx-only: per vertex u8 color x4 + half uv x2 (BE)

Compressed indexes (flavor 3, the only one in the data), decompressor at EBOOT SPU code 0x00D5F4B0:
  u16 numExplicit, u16 base, u16 maskBytes, u8 explicitBits, u8 0
  mask (maskBytes): 1 bit per vertex slot, MSB first; 1 = explicit index, 0 = next new vertex
  2 bits per triangle: 3 = three new slots; 0/1/2 = reuse edge (p2,p0) / (p1,p2) / (p0,p1)
    of the previous triangle p, built as (edge[1], edge[0], slot)
  byte-aligned: numExplicit values of explicitBits bits, decoded as
    index[i] = (value[i] - base) + index[i - 8]   (eight interleaved delta streams)
"""
import struct


def _align16(x):
    return (x + 15) & ~15


def _snorm(v, bits):
    v &= (1 << bits) - 1
    if v >> (bits - 1):
        v -= 1 << bits
    return max(-1.0, v / ((1 << (bits - 1)) - 1))


def unpack_x11y11z10(v):
    return _snorm(v, 11), _snorm(v >> 11, 11), _snorm(v >> 22, 10)


class _Bits:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def read(self, n):
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3] if (self.pos >> 3) < len(self.data) else 0
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def align(self):
        self.pos = (self.pos + 7) & ~7


def decompress_indexes(data, num_indexes):
    num_explicit, base, mask_bytes = struct.unpack_from(">HHH", data, 0)
    width = data[6]
    mask = _Bits(data[8:8 + mask_bytes])
    stream = _Bits(data[8 + mask_bytes:])
    num_tris = num_indexes // 3
    codes = [stream.read(2) for _ in range(num_tris)]
    stream.align()
    explicit = [stream.read(width) for _ in range(num_explicit)]
    for i in range(num_explicit):
        explicit[i] = (explicit[i] - base + (explicit[i - 8] if i >= 8 else 0)) & 0xFFFF
    edges = ((2, 0), (1, 2), (0, 1))
    out, nxt, ei, prev = [], 0, 0, None

    def slot():
        nonlocal nxt, ei
        if mask.read(1):
            ei += 1
            return explicit[ei - 1]
        nxt += 1
        return nxt - 1

    for code in codes:
        if code == 3 or prev is None:
            tri = (slot(), slot(), slot())
        else:
            i, j = edges[code]
            tri = (prev[j], prev[i], slot())
        out.extend(tri)
        prev = tri
    return out


class Segment:
    __slots__ = ("nv", "indices", "pos", "normal", "uv", "color", "joints", "weights", "fmt")


def parse_medg(data, vtx_scale, stats=None):
    """Returns the decoded segments of one MEDG payload (skips unsupported ones)."""
    count = struct.unpack_from("<I", data, 0)[0]
    o, segments = 4, []
    for _ in range(count):
        sz = struct.unpack_from("<14H", data, o)
        rsx_size = struct.unpack_from("<I", data, o + 28)[0]
        cfg = data[o + 40:o + 56]
        blob = o + 56
        nv, ni = struct.unpack_from(">HH", cfg, 8)
        p_idx = blob
        p_s0 = p_idx + _align16(sz[0] + sz[1])
        p_s1 = p_s0 + _align16(sz[2] + sz[3] + sz[4])
        p_skin = p_s1 + _align16(sz[5] + sz[6] + sz[7])
        p_rsx = p_skin + sz[12] + sz[13]
        o = p_rsx + rsx_size
        seg = _decode_segment(data, cfg, sz, nv, ni, p_idx, p_s0, p_s1, p_skin, p_rsx, rsx_size, vtx_scale, stats)
        if seg is not None:
            segments.append(seg)
    return segments


def _decode_segment(data, cfg, sz, nv, ni, p_idx, p_s0, p_s1, p_skin, p_rsx, rsx_size, vtx_scale, stats):
    def stat(key):
        if stats is not None:
            stats[key] += 1

    fmt, flavor = cfg[2], cfg[6] >> 4
    seg = Segment()
    seg.nv, seg.fmt = nv, fmt
    idx = data[p_idx:p_idx + sz[0] + sz[1]]
    # only flavor 3 (compressed) occurs in the game data; the rest is reported, not guessed
    if flavor != 3:
        stat(f"edge_flavor_{flavor}")
        return None
    seg.indices = decompress_indexes(idx, ni)
    s0 = data[p_s0:p_s0 + sz[2]]
    if fmt == 0 and len(s0) >= 12 * nv:
        seg.pos = [struct.unpack_from(">3f", s0, 12 * i) for i in range(nv)]
    elif fmt == 7 and len(s0) >= 4 * nv:
        seg.pos = [tuple(c * vtx_scale for c in unpack_x11y11z10(struct.unpack_from(">I", s0, 4 * i)[0]))
                   for i in range(nv)]
    else:
        stat(f"edge_formato_{fmt}")
        return None
    s1 = data[p_s1:p_s1 + sz[5]]
    stride = 12 if len(s1) >= 12 * nv else 4 if len(s1) >= 4 * nv else 0
    seg.normal = [unpack_x11y11z10(struct.unpack_from(">I", s1, stride * i)[0]) for i in range(nv)] if stride else None
    seg.uv = seg.color = None
    if rsx_size >= 8 * nv:
        rs = rsx_size // nv
        seg.color = [tuple(data[p_rsx + rs * i:p_rsx + rs * i + 4]) for i in range(nv)]
        seg.uv = [struct.unpack_from(">2e", data, p_rsx + rs * i + 4) for i in range(nv)]
    seg.joints = seg.weights = None
    if sz[12] >= 8 * nv:
        off = (sz[8] // 48, sz[9] // 48)
        n0 = sz[10] // 48
        joints, weights = [], []
        for i in range(nv):
            b = data[p_skin + 8 * i:p_skin + 8 * i + 8]
            w = [b[0], b[2], b[4], b[6]]
            j = [off[0] + x if x < n0 else off[1] + (x - n0) for x in (b[1], b[3], b[5], b[7])]
            joints.append(tuple(jj if ww else 0 for jj, ww in zip(j, w)))
            weights.append(tuple(w))
        seg.joints, seg.weights = joints, weights
    stat("edge_segmentos")
    return seg
