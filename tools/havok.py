"""Reader for Havok binary packfiles (hk_2011.3.0-r1, 4-byte pointers, big-endian)
as stored in HKAD / HKSD chunks, plus decoders for the animation classes.

Packfile: 0x40-byte header (magic 57E0E057 10C0C010, fileVersion, layout rules,
numSections, contents section/offset, version string), then 0x30-byte section
headers: char tag[20], u32 absoluteDataStart and offsets (relative to it) of
localFixups, globalFixups, virtualFixups, exports, imports, end.
  local fixups  : (u32 src, u32 dst)            pointer inside the same section
  global fixups : (u32 src, u32 section, u32 dst) pointer into another section
  virtual fixups: (u32 objOffset, u32 section, u32 classNameOffset)  -> object start + class
"""
import struct


class Packfile:
    def __init__(self, data):
        self.d = data
        m0, m1 = struct.unpack_from(">II", data, 0)
        if (m0, m1) != (0x57E0E057, 0x10C0C010):
            raise ValueError("not a Havok packfile")
        self.version = data[0x28:0x38].split(b"\0")[0].decode("ascii", "replace")
        self.ptr_size, self.little = data[0x10], data[0x11]
        if self.ptr_size != 4 or self.little:
            raise ValueError("only 4-byte big-endian packfiles are supported")
        nsec = struct.unpack_from(">I", data, 0x14)[0]
        self.sections = []
        for i in range(nsec):
            o = 0x40 + 0x30 * i
            tag = data[o:o + 20].split(b"\0")[0].decode("ascii", "replace")
            start, local, glob, virt, exp, imp, end = struct.unpack_from(">7I", data, o + 20)
            self.sections.append({"tag": tag, "start": start, "local": local, "global": glob, "virtual": virt,
                                  "exports": exp, "imports": imp, "end": end})
        self.pointers = {}  # absolute src -> absolute dst
        self.objects = []   # (absolute offset, class name)
        def entries(lo, hi, size):
            # tables are padded with 0xFF up to 16 bytes; never read past the buffer
            return range(lo, min(hi, len(data)) - size + 1, size)

        for sec in self.sections:
            s = sec["start"]
            for o in entries(s + sec["local"], s + sec["global"], 8):
                src, dst = struct.unpack_from(">II", data, o)
                if src != 0xFFFFFFFF:
                    self.pointers[s + src] = s + dst
            for o in entries(s + sec["global"], s + sec["virtual"], 12):
                src, sid, dst = struct.unpack_from(">III", data, o)
                if src != 0xFFFFFFFF:
                    self.pointers[s + src] = self.sections[sid]["start"] + dst
            for o in entries(s + sec["virtual"], s + sec["exports"], 12):
                src, sid, name = struct.unpack_from(">III", data, o)
                if src != 0xFFFFFFFF:
                    n0 = self.sections[sid]["start"] + name
                    cls = data[n0:data.index(b"\0", n0)].decode("ascii", "replace")
                    self.objects.append((s + src, cls))
        self.objects.sort()

    # ---- raw access helpers (absolute offsets) ----
    def u8(self, o):
        return self.d[o]

    def u16(self, o):
        return struct.unpack_from(">H", self.d, o)[0]

    def s16(self, o):
        return struct.unpack_from(">h", self.d, o)[0]

    def u32(self, o):
        return struct.unpack_from(">I", self.d, o)[0]

    def s32(self, o):
        return struct.unpack_from(">i", self.d, o)[0]

    def f32(self, o):
        return struct.unpack_from(">f", self.d, o)[0]

    def ptr(self, o):
        return self.pointers.get(o)

    def array(self, o):
        """hkArray at o: (data pointer or None, size)."""
        return self.ptr(o), self.s32(o + 4)

    def cstring(self, o):
        p = self.ptr(o)
        if p is None:
            return None
        return self.d[p:self.d.index(b"\0", p)].decode("utf-8", "replace")

    def find(self, cls):
        return [o for o, c in self.objects if c == cls]


# ---------------------------------------------------------------- hka classes (2011.3, 4-byte pointers)

def qs_transform(pf, o):
    """hkQsTransform (48 bytes): translation vec4, rotation quaternion (x, y, z, w), scale vec4."""
    t = struct.unpack_from(">3f", pf.d, o)
    r = struct.unpack_from(">4f", pf.d, o + 16)
    s = struct.unpack_from(">3f", pf.d, o + 32)
    return t, r, s


def read_skeleton(pf, o):
    """hkaSkeleton: +08 name, +0C parentIndices hkArray<s16>, +18 bones hkArray<hkaBone{name, lock}>,
    +24 referencePose hkArray<hkQsTransform>."""
    name = pf.cstring(o + 8)
    pp, pn = pf.array(o + 0x0C)
    bp, bn = pf.array(o + 0x18)
    rp, rn = pf.array(o + 0x24)
    parents = [pf.s16(pp + 2 * i) for i in range(pn)] if pp is not None else []
    bones = [pf.cstring(bp + 8 * i) for i in range(bn)] if bp is not None else []
    pose = [qs_transform(pf, rp + 48 * i) for i in range(rn)] if rp is not None else []
    return {"name": name, "parents": parents, "bones": bones, "pose": pose}


# ---------------------------------------------------------------- spline-compressed animation

ROT_SIZE = {0: 4, 1: 5, 2: 6, 3: 3, 4: 2, 5: 16}   # POLAR32, THREECOMP40, THREECOMP48, THREECOMP24, STRAIGHT16, UNCOMPRESSED
ROT_ALIGN = {0: 4, 1: 1, 2: 2, 3: 1, 4: 2, 5: 4}
SQRT1_2 = 0.70710678118


# The spline data blob is little-endian even on PS3 (hkaSplineCompressedAnimation.endian == 0),
# unlike the packfile structures around it.
def _quat_40(b):
    v = int.from_bytes(b, "little")
    mask = (1 << 12) - 1
    half = mask >> 1
    comps = [((v >> s) & mask) - half for s in (0, 12, 24)]
    shift = (v >> 36) & 3
    invert = (v >> 38) & 1
    xyz = [c * (SQRT1_2 / half) for c in comps]
    w = max(0.0, 1.0 - sum(c * c for c in xyz)) ** 0.5
    if invert:
        w = -w
    xyz.insert(shift, w)
    return tuple(xyz)


def _quat_48(b):
    x, y, z = struct.unpack("<3H", b)
    shift = ((y >> 14) & 2) | ((x >> 15) & 1)
    invert = (z >> 15) & 1
    mask = (1 << 15) - 1
    half = mask >> 1
    xyz = [((c & mask) - half) * (SQRT1_2 / half) for c in (x, y, z)]
    w = max(0.0, 1.0 - sum(c * c for c in xyz)) ** 0.5
    if invert:
        w = -w
    xyz.insert(shift, w)
    return tuple(xyz)


def _quat_24(b):
    """One byte per stored component: 7-bit value (v - 63) * sqrt(1/2) / 63 plus a flag in bit 7:
    byte 0 = low bit and byte 1 = high bit of the omitted component's position, byte 2 = its sign.
    Read from the game's own encoder (EBOOT 0x00AB9E44): value = int(c * sqrt(2) * 63) + 63."""
    shift = ((b[1] >> 6) & 2) | (b[0] >> 7)
    invert = b[2] >> 7
    xyz = [((c & 127) - 63) * (SQRT1_2 / 63) for c in b[:3]]
    w = max(0.0, 1.0 - sum(c * c for c in xyz)) ** 0.5
    if invert:
        w = -w
    xyz.insert(shift, w)
    return tuple(xyz)


def _quat_16(b):
    """All four components as 4-bit nibbles (n - 7) / 7, then normalized:
    byte 0 = x | y << 4, byte 1 = z | w << 4 (EBOOT decoder 0x00ABAE2C, encoder 0x00AB9C8C)."""
    q = [((b[0] & 15) - 7) / 7, ((b[0] >> 4) - 7) / 7, ((b[1] & 15) - 7) / 7, ((b[1] >> 4) - 7) / 7]
    n = sum(c * c for c in q) ** 0.5
    return tuple(c / n for c in q) if n else (0.0, 0.0, 0.0, 1.0)


def _quat(b, kind):
    if kind == 1:
        return _quat_40(b)
    if kind == 2:
        return _quat_48(b)
    if kind == 3:
        return _quat_24(b)
    if kind == 4:
        return _quat_16(b)
    if kind == 5:
        return struct.unpack("<4f", b)
    raise NotImplementedError(f"quaternion type {kind}")


class _Cursor:
    def __init__(self, data, pos):
        self.d, self.p = data, pos

    def align(self, n):
        self.p = (self.p + n - 1) // n * n

    def take(self, n):
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def f32(self):
        return struct.unpack("<f", self.take(4))[0]


class Track:
    """One curve (vector or quaternion): constant value or B-spline over the block's frames."""
    __slots__ = ("const", "degree", "knots", "points")

    def __init__(self, const=None, degree=0, knots=None, points=None):
        self.const, self.degree, self.knots, self.points = const, degree, knots, points

    def eval(self, t):
        if self.const is not None:
            return self.const
        p, U, P = self.degree, self.knots, self.points
        n = len(P) - 1
        # knot span: largest k in [p, n] with U[k] <= t
        k = p
        while k < n and U[k + 1] <= t:
            k += 1
        # de Boor
        d = [list(P[j + k - p]) for j in range(p + 1)]
        for r in range(1, p + 1):
            for j in range(p, r - 1, -1):
                i = j + k - p
                den = U[i + p - r + 1] - U[i]
                a = (t - U[i]) / den if den else 0.0
                d[j] = [(1 - a) * x + a * y for x, y in zip(d[j - 1], d[j])]
        return tuple(d[p])


def _read_vector(cur, mask, quant, default):
    bits = 8 if quant == 0 else 16
    if mask & 0xF0:
        n = cur.u16()
        degree = cur.u8()
        knots = list(cur.take(n + degree + 2))
        cur.align(4)
        ranges = []
        for c in range(3):
            if mask & (0x10 << c):
                lo, hi = cur.f32(), cur.f32()
                ranges.append(("s", lo, hi))
            elif mask & (1 << c):
                ranges.append(("c", cur.f32()))
            else:
                ranges.append(("c", default))
        points = []
        scale = (1 << bits) - 1
        for _ in range(n + 1):
            pt = []
            for r in ranges:
                if r[0] == "s":
                    q = cur.u8() if bits == 8 else cur.u16()
                    pt.append(r[1] + (r[2] - r[1]) * q / scale)
                else:
                    pt.append(r[1])
            points.append(tuple(pt))
        cur.align(4)
        return Track(degree=degree, knots=knots, points=points)
    if mask & 0x0F:
        return Track(const=tuple(cur.f32() if mask & (1 << c) else default for c in range(3)))
    return Track(const=(default,) * 3)


def _read_quat(cur, mask, kind):
    size = ROT_SIZE[kind]
    if mask & 0xF0:
        n = cur.u16()
        degree = cur.u8()
        knots = list(cur.take(n + degree + 2))
        cur.align(ROT_ALIGN[kind])
        points = [_quat(cur.take(size), kind) for _ in range(n + 1)]
        cur.align(4)
        return Track(degree=degree, knots=knots, points=points)
    if mask & 0x0F:
        cur.align(ROT_ALIGN[kind])
        q = _quat(cur.take(size), kind)
        cur.align(4)
        return Track(const=q)
    return Track(const=(0.0, 0.0, 0.0, 1.0))


class SplineAnimation:
    """hkaSplineCompressedAnimation: hkaAnimation (+08 type, +0C duration, +10 numberOfTransformTracks,
    +14 numberOfFloatTracks, +1C annotationTracks) then +28 numFrames, +2C numBlocks,
    +30 maxFramesPerBlock, +34 maskAndQuantizationSize, +38 blockDuration,
    +3C blockInverseDuration, +40 frameDuration, +44 blockOffsets, +50 floatBlockOffsets,
    +5C transformOffsets, +68 floatOffsets, +74 data, +80 endian."""

    def __init__(self, pf, o):
        self.duration = pf.f32(o + 0x0C)
        self.num_tracks = pf.s32(o + 0x10)
        self.num_floats = pf.s32(o + 0x14)
        self.num_frames = pf.s32(o + 0x28)
        self.num_blocks = pf.s32(o + 0x2C)
        self.frames_per_block = pf.s32(o + 0x30)
        self.mask_size = pf.s32(o + 0x34)
        self.frame_duration = pf.f32(o + 0x40)
        bp, bn = pf.array(o + 0x44)
        dp, _dn = pf.array(o + 0x74)
        self.blocks = []
        for b in range(bn):
            start = dp + pf.u32(bp + 4 * b)
            masks = pf.d[start:start + 4 * self.num_tracks]
            cur = _Cursor(pf.d, start + self.mask_size)
            cur.align(4)
            tracks = []
            for t in range(self.num_tracks):
                quant, pmask, rmask, smask = masks[4 * t:4 * t + 4]
                pos = _read_vector(cur, pmask, quant & 3, 0.0)
                rot = _read_quat(cur, rmask, (quant >> 2) & 15)
                scl = _read_vector(cur, smask, (quant >> 6) & 3, 1.0)
                tracks.append((pos, rot, scl))
            self.blocks.append(tracks)

    def sample(self, frame):
        """Local transforms (translation, rotation xyzw, scale) of every track at a frame."""
        span = self.frames_per_block - 1
        b = min(int(frame // span), self.num_blocks - 1)
        t = frame - b * span
        return [(p.eval(t), r.eval(t), s.eval(t)) for p, r, s in self.blocks[b]]


class InterleavedAnimation:
    """hkaInterleavedUncompressedAnimation: hkaAnimation + +28 transforms hkArray<hkQsTransform>
    (numFrames x numTracks, frame-major) + +34 floats."""

    def __init__(self, pf, o):
        self.duration = pf.f32(o + 0x0C)
        self.num_tracks = pf.s32(o + 0x10)
        tp, tn = pf.array(o + 0x28)
        self.num_frames = tn // max(self.num_tracks, 1)
        self.frame_duration = self.duration / max(self.num_frames - 1, 1)
        self.frames = [[qs_transform(pf, tp + 48 * (f * self.num_tracks + t)) for t in range(self.num_tracks)]
                       for f in range(self.num_frames)]

    def sample(self, frame):
        return self.frames[min(int(round(frame)), self.num_frames - 1)]


def read_animation(pf, o):
    cls = next(c for oo, c in pf.objects if oo == o)
    if cls == "hkaSplineCompressedAnimation":
        return SplineAnimation(pf, o)
    if cls == "hkaInterleavedUncompressedAnimation":
        return InterleavedAnimation(pf, o)
    raise NotImplementedError(cls)


def read_binding(pf, o):
    """hkaAnimationBinding: +08 originalSkeletonName, +0C animation, +10 transformTrackToBoneIndices
    hkArray<s16>, +1C floatTrackToFloatSlotIndices, +28 blendHint."""
    tp, tn = pf.array(o + 0x10)
    return {"skeleton": pf.cstring(o + 8), "animation": pf.ptr(o + 0x0C),
            "track_to_bone": [pf.s16(tp + 2 * i) for i in range(tn)] if tp is not None else [],
            "blend_hint": pf.s32(o + 0x28)}
