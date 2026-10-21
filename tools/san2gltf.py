"""Exports meshes from .san / .san.cooked files to glTF 2.0 (and optionally OBJ).

Chunk layouts come from the game's own chunk handlers (EBOOT, see
docs/GHIDRA.md); header fields are little-endian, vertex/index data is
big-endian (already cooked for the RSX).

  MESH = MSHH + SMSH...           (AglTopology, handler 0x00525E40)
    MSHH: u8 ver, vec3 center, f32 radius, u32 hash, u8 vertexCompressionMode
    SMSH = MHDR + MVTX + MIDX     (AglSubMesh)
      MHDR v4: u8 ver, u16 numVerts, u16 numIndices, u8 primitiveType,
               u32 materialHash, u32 vtxDefHash, f32 vtxScale, u8 visGroup,
               u32 vtxPackBufOfst/Size, vec4 boundSphere, u32 idxPackBufOfst/Size,
               u32 stream1PackBufOfst/Size, then elements of 10 bytes:
               u8 usage, usageIndex, type, size, fract, stride; u32 offset
      MVTX: interleaved vertices (stride from the elements)
      MIDX: u16 indices (triangle list)
  Element type -> RSX format (table at EBOOT 0x00F67A54):
      0 f32, 2 u8, 4 u8 normalized, 7/25 s16 normalized, 24 half, 26 CMP 11:11:10
  Element usage: 0 position, 1 blend weight, 2 blend indices, 3 normal,
      5 texcoord, 6 tangent, 7 binormal, 9 color
  MATL/MTLH v4: u8 ver, u32 diffuse/envMap/normalMap/specularMap/decalMap hash,
      u32 nameHash, char name[32], u8 blendMode, cullMode, flags, ...
  TEXR/TXRH: u32 nameHash at offset 1 (see texr2dds.py)
  GEOB = GEOH (u8 ver, vec3 bboxMin/Max, u32 nameHash, animHash, lightGroup,
      char name[32], ...) + GLOD... (u8 ver, u32 meshHash, f32 switchDist)
  GINS = GINH + child GINS... (instances; child transforms are local)
    GINH: u8, u8 version, vec3 xBasis, zBasis, position, scale, u32 flags,
      nameHash, animHash, char name[32], u32 defHash (-> GEOH.nameHash), ...
Links: GINH.defHash -> GEOH.nameHash -> GLOD.meshHash -> MSHH.hash;
       MHDR.materialHash -> MTLH.nameHash -> diffuseHash -> TXRH.nameHash.
Skinned meshes: SMSH = MHDR + MEDG (PlayStation EDGE, decoded by edge.py; see
docs/EDGE.md) and their GEOB carries SKEL (SKHD + BONS); they are exported with a
glTF skin (one joint node per bone, JOINTS_0/WEIGHTS_0, inverse bind matrices).
Animations: HKAF = HKAH (clip name) + HKAD (Havok 2011.3 packfile, decoded by
havok.py; see docs/HAVOK.md). --mode characters writes one glTF per skinned
GEOB with every clip whose Havok skeleton matches its BONS by bone name.

Verified against the data (Paris/Hideout): every hash link resolves, positions
fit the submesh bounding spheres without vtxScale (vertexCompressionMode 0),
CMP normals have unit length, triangle winding agrees with the normals, and a
textured keyboard reads correctly, so the world is used as-is (no mirroring).
Inferred (not read from code): blendMode 0 opaque / 2 alpha blend / 3 additive,
cullMode 2 = two-sided. Raw values are kept in each material's "extras".
Vertex colors are often black (shader data, not a tint), so they are written as
the custom attribute _COLOR_RAW unless --vertex-colors is given.

Usage:
  san2gltf.py INPUT... [--out DIR] [--mode scene|meshes] [--obj] [--no-textures] [--vertex-colors]
    scene  : instance hierarchy as placed in the level (default)
    meshes : every mesh once, at the origin
"""
import argparse
import io
import json
import math
import os
import re
import struct
import sys
from collections import Counter

import edge
import havok
import sanchunks
import texr2dds

USAGE_POS, USAGE_NORMAL, USAGE_UV, USAGE_COLOR = 0, 3, 5, 9
# engine element type -> (struct char, decode)
TYPE_HALF, TYPE_F32, TYPE_U8, TYPE_U8N, TYPE_S16N, TYPE_S16N_B, TYPE_CMP = 24, 0, 2, 4, 7, 25, 26


def cstr(b):
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


def decode_cmp(v):
    """RSX CELL_GCM_VERTEX_CMP: x 11 bits, y 11 bits, z 10 bits, signed normalized."""
    def s(x, bits):
        x &= (1 << bits) - 1
        if x >> (bits - 1):
            x -= 1 << bits
        return max(-1.0, x / ((1 << (bits - 1)) - 1))
    return s(v, 11), s(v >> 11, 11), s(v >> 22, 10)


def basis_matrix(xb, zb, pos, scl):
    """Column-major 4x4 from x/z basis vectors (y = z cross x), position and scale."""
    yb = (zb[1] * xb[2] - zb[2] * xb[1], zb[2] * xb[0] - zb[0] * xb[2], zb[0] * xb[1] - zb[1] * xb[0])
    return [*(v * scl[0] for v in xb), 0, *(v * scl[1] for v in yb), 0, *(v * scl[2] for v in zb), 0, *pos, 1]


def mat_mul(a, b):
    """Column-major 4x4 product a*b."""
    return [sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4)) for c in range(4) for r in range(4)]


def mat_inverse(m):
    """Inverse of a column-major affine 4x4."""
    a, b, c = m[0], m[4], m[8]
    d, e, f = m[1], m[5], m[9]
    g, h, i = m[2], m[6], m[10]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1e-12:
        return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    r = [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
         (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
         (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]  # row-major 3x3 inverse
    t = (m[12], m[13], m[14])
    tx = -(r[0] * t[0] + r[1] * t[1] + r[2] * t[2])
    ty = -(r[3] * t[0] + r[4] * t[1] + r[5] * t[2])
    tz = -(r[6] * t[0] + r[7] * t[1] + r[8] * t[2])
    return [r[0], r[3], r[6], 0, r[1], r[4], r[7], 0, r[2], r[5], r[8], 0, tx, ty, tz, 1]


# ---------------------------------------------------------------- parsing

class Scene:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.buf = f.read()
        self.path = path
        self.textures = {}   # nameHash -> (name, Texture)
        self.materials = {}  # nameHash -> dict
        self.meshes = {}     # hash -> list of submesh dicts
        self.mesh_names = {}
        self.geos = {}       # nameHash -> (name, meshHash or None)
        self.roots = []      # instance trees
        self.hk_skeletons = {}  # Havok skeleton name -> read_skeleton dict (HKSF)
        self.clips = []         # (clip name, HKAD bytes) from HKAF, decoded on demand
        self.stats = Counter()
        self._parse()

    def p(self, c):
        return self.buf[c.data_start:c.end]

    def _parse(self):
        top = [c for c in sanchunks.walk(self.buf, max_depth=2) if c.path.count("/") == 3]  # /FORM/PROF/X
        for c in top:
            if c.tag == "TEXR":
                self._texture(c)
            elif c.tag == "MATL":
                self._material(c)
            elif c.tag == "MESH":
                self._mesh(c)
            elif c.tag == "GEOB":
                self._geo(c)
            elif c.tag == "GINS":
                self.roots.append(self._instance(c))
            elif c.tag in ("HKAF", "HKSF"):
                self._havok(c)

    def _havok(self, c):
        """HKSF = HKSH + HKSD (Havok skeleton packfile); HKAF = HKAH (u8, char clip[64],
        char sourceFile[64]) + HKAD (Havok animation packfile)."""
        parts = {k.tag: self.p(k) for k in sanchunks.children(self.buf, c)}
        if c.tag == "HKSF" and "HKSD" in parts:
            try:
                pf = havok.Packfile(parts["HKSD"])
                for o in pf.find("hkaSkeleton"):
                    sk = havok.read_skeleton(pf, o)
                    self.hk_skeletons.setdefault(sk["name"], sk)
            except (ValueError, struct.error):
                self.stats["havok_esqueleto_ilegivel"] += 1
        elif c.tag == "HKAF" and "HKAD" in parts:
            name = cstr(parts.get("HKAH", b"\0" * 65)[1:65]) or f"clip_{len(self.clips)}"
            self.clips.append((name, parts["HKAD"]))

    def _bound_clips(self):
        """(clip name, packfile, binding, Havok skeleton) of every readable clip, parsed once."""
        if not hasattr(self, "_bound"):
            self._bound = []
            for clip_name, data in self.clips:
                try:
                    pf = havok.Packfile(data)
                    binding = havok.read_binding(pf, pf.find("hkaAnimationBinding")[0])
                except (ValueError, IndexError, struct.error):
                    self.stats["havok_clip_ilegivel"] += 1
                    continue
                hk = self.hk_skeletons.get(binding["skeleton"])
                if hk is None:
                    pf_skel = pf.find("hkaSkeleton")  # some clips embed their skeleton
                    hk = havok.read_skeleton(pf, pf_skel[0]) if pf_skel else None
                if hk is not None and hk["bones"]:
                    self._bound.append((clip_name, pf, binding, hk))
        return self._bound

    def clips_for(self, bones):
        """Decoded clips whose Havok skeleton matches these BONS by bone name:
        yields (clip name, animation, [our joint index per track or None]).
        Generic names (Root, Pelvis, Head...) make small skeletons such as a bat's fit
        inside a guard's, so the skeleton must also be (nearly) the best match by
        Jaccard overlap of the two name sets, not just covered by them."""
        names = {b["name"]: i for i, b in enumerate(bones)}
        bound = self._bound_clips()
        score = {}
        for _c, _pf, _b, hk in bound:
            if hk["name"] not in score:
                hset = set(hk["bones"])
                common = len(hset & names.keys())
                score[hk["name"]] = (common / len(hset), common / len(hset | names.keys()))
        best = max((j for _cov, j in score.values()), default=0)
        for clip_name, pf, binding, hk in bound:
            coverage, jaccard = score[hk["name"]]
            if coverage < 0.9 or jaccard < 0.9 * best:
                continue
            try:
                anim = havok.read_animation(pf, binding["animation"])
            except (NotImplementedError, StopIteration, struct.error, IndexError) as e:
                self.stats[f"havok_clip_nao_suportado: {type(e).__name__}"] += 1
                continue
            track_map = []
            for t in range(anim.num_tracks):
                hb = binding["track_to_bone"][t] if t < len(binding["track_to_bone"]) else t
                track_map.append(names.get(hk["bones"][hb]) if 0 <= hb < len(hk["bones"]) else None)
            yield clip_name, anim, track_map

    def _texture(self, c):
        hdr, gtf = {}, None
        for k in sanchunks.children(self.buf, c):
            if k.tag == "TXRH":
                hdr = texr2dds.parse_txrh(self.p(k))
            elif k.tag == "TGTF":
                gtf = self.p(k)
        if gtf and hdr:
            tex = texr2dds.parse_gtf(gtf, self.path, hdr["name"] or f"tex_{hdr['nameHash']:08X}")
            if tex:
                self.textures[hdr["nameHash"]] = (tex[0].name, tex[0])

    def _material(self, c):
        for k in sanchunks.children(self.buf, c):
            if k.tag == "MTLH":
                b = self.p(k)
                if b[0] < 3:
                    self.stats["material_old_version"] += 1
                    continue
                diffuse, env, normal, spec, decal, name_hash = struct.unpack_from("<6I", b, 1)
                blend, cull, flags = b[57], b[58], b[59]
                diffuse_color = tuple(b[60:64])
                self.materials[name_hash] = {
                    "name": cstr(b[25:57]) or f"mat_{name_hash:08X}", "diffuse": diffuse, "normal": normal,
                    "specular": spec, "blend": blend, "cull": cull, "flags": flags, "color": diffuse_color}
                self.stats[f"blendMode={blend}"] += 1
                self.stats[f"cullMode={cull}"] += 1

    def _mesh(self, c):
        kids = sanchunks.children(self.buf, c)
        mshh = next((k for k in kids if k.tag == "MSHH"), None)
        if mshh is None:
            return
        b = self.p(mshh)
        mesh_hash = struct.unpack_from("<I", b, 17)[0]
        mode = b[21]
        subs = []
        for s in kids:
            if s.tag != "SMSH":
                continue
            parts = {k.tag: k for k in sanchunks.children(self.buf, s)}
            if "MHDR" in parts and "MEDG" in parts:
                sub = self._edge_submesh(self.p(parts["MHDR"]), self.p(parts["MEDG"]))
            elif {"MHDR", "MVTX", "MIDX"} <= set(parts):
                sub = self._submesh(self.p(parts["MHDR"]), self.p(parts["MVTX"]), self.p(parts["MIDX"]), mode)
            else:
                self.stats["submesh_without_vertices"] += 1
                continue
            if sub:
                subs.append(sub)
        self.meshes[mesh_hash] = subs

    def _edge_submesh(self, h, medg):
        """Skinned/EDGE submesh: all MEDG segments merged into one primitive."""
        material, _vtxdef, vtx_scale = struct.unpack_from("<IIf", h, 6)
        out = {"material": material, "pos": [], "normal": [], "uv": [], "color": [], "joints": [], "weights": [],
               "indices": []}
        segs = edge.parse_medg(medg, vtx_scale, self.stats)
        if not segs:
            return None
        have = {k: all(getattr(sg, k) is not None for sg in segs) for k in ("normal", "uv", "color", "joints")}
        for sg in segs:
            base = len(out["pos"])
            out["indices"] += [base + i for i in sg.indices]
            out["pos"] += sg.pos
            for k in ("normal", "uv", "color"):
                if have[k]:
                    out[k] += getattr(sg, k)
            if have["joints"]:
                out["joints"] += sg.joints
                out["weights"] += sg.weights
        for k in ("normal", "uv", "color", "joints", "weights"):
            if not out[k]:
                out[k] = None
        self.stats["submeshes_edge"] += 1
        self.stats["vertices"] += len(out["pos"])
        self.stats["triangles"] += len(out["indices"]) // 3
        return out

    def _submesh(self, h, vtx, idx, compression_mode):
        ver = h[0]
        if ver < 4:
            self.stats[f"MHDR_v{ver}_ignorado"] += 1
            return None
        nv, ni = struct.unpack_from("<HH", h, 1)
        prim = h[5]
        material, _vtxdef, vtx_scale = struct.unpack_from("<IIf", h, 6)
        els = [struct.unpack_from("<BBBBBBI", h, 59 + 10 * i) for i in range((len(h) - 59) // 10)]
        stride = els[0][5] if els else 0
        if not stride or len(vtx) < nv * stride:
            self.stats["vertex_buffer_curto"] += 1
            return None
        out = {"material": material, "pos": None, "normal": None, "uv": None, "color": None, "joints": None,
               "weights": None}
        for usage, uidx, typ, size, _fract, _st, off in els:
            key = {USAGE_POS: "pos", USAGE_NORMAL: "normal", USAGE_UV: "uv", USAGE_COLOR: "color"}.get(usage)
            if key is None or uidx != 0:
                continue
            vals = self._read_element(vtx, nv, stride, off, typ, size)
            if vals is None:
                self.stats[f"tipo_nao_suportado usage={usage} type={typ} size={size}"] += 1
                continue
            if key == "pos" and compression_mode != 0:
                vals = [tuple(x * vtx_scale for x in v) for v in vals]
            out[key] = vals
        if out["pos"] is None:
            return None
        indices = list(struct.unpack_from(f">{ni}H", idx, 0))
        if prim == 1:  # strip -> list (engine maps prim != 0 to RSX triangle strip)
            tri = []
            for i in range(len(indices) - 2):
                a, b, c = indices[i:i + 3]
                if a != b and b != c and a != c:
                    tri += [a, b, c] if i % 2 == 0 else [b, a, c]
            indices = tri
        out["indices"] = indices
        self.stats["submeshes"] += 1
        self.stats["vertices"] += nv
        self.stats["triangles"] += len(indices) // 3
        return out

    def _read_element(self, vtx, nv, stride, off, typ, size):
        if typ == TYPE_HALF:
            fmt = f">{size}e"
        elif typ == TYPE_F32:
            fmt = f">{size}f"
        elif typ in (TYPE_U8, TYPE_U8N):
            fmt = f">{size}B"
        elif typ in (TYPE_S16N, TYPE_S16N_B):
            fmt = f">{size}h"
        elif typ == TYPE_CMP and size == 1:
            fmt = ">I"
        else:
            return None
        st = struct.Struct(fmt)
        vals = [st.unpack_from(vtx, i * stride + off) for i in range(nv)]
        if typ == TYPE_CMP:
            return [decode_cmp(v[0]) for v in vals]
        if typ in (TYPE_S16N, TYPE_S16N_B):
            return [tuple(max(-1.0, x / 32767) for x in v) for v in vals]
        return vals

    def _geo(self, c):
        name, mesh, skeleton = None, None, None
        for k in sanchunks.children(self.buf, c):
            b = self.p(k)
            if k.tag == "GEOH":
                name_hash = struct.unpack_from("<I", b, 25)[0]
                name = cstr(b[37:69])
            elif k.tag == "GLOD" and mesh is None:  # first LOD = most detailed
                mesh = struct.unpack_from("<I", b, 1)[0]
            elif k.tag == "SKEL":
                skeleton = self._skeleton(k)
        if name is not None:
            self.geos[name_hash] = (name, mesh, skeleton)
            if mesh is not None:
                self.mesh_names.setdefault(mesh, name)

    def _skeleton(self, c):
        """SKEL = SKHD (u8 ver, u32 numBones) + BONS (76-byte bones: nameHash, parentNameHash,
        vec3 xBasis, zBasis, position, char name[32]); transforms are absolute (bind pose)."""
        parts = {k.tag: self.p(k) for k in sanchunks.children(self.buf, c)}
        if "SKHD" not in parts or "BONS" not in parts:
            return None
        count = min(struct.unpack_from("<I", parts["SKHD"], 1)[0], len(parts["BONS"]) // 76)
        bones = []
        for i in range(count):
            o = 76 * i
            name_hash, parent_hash = struct.unpack_from("<II", parts["BONS"], o)
            xb = struct.unpack_from("<3f", parts["BONS"], o + 8)
            zb = struct.unpack_from("<3f", parts["BONS"], o + 20)
            pos = struct.unpack_from("<3f", parts["BONS"], o + 32)
            bones.append({"name": cstr(parts["BONS"][o + 44:o + 76]) or f"bone_{i}", "hash": name_hash,
                          "parent_hash": parent_hash, "world": basis_matrix(xb, zb, pos, (1, 1, 1))})
        index = {b["hash"]: i for i, b in enumerate(bones)}
        for i, b in enumerate(bones):
            p = index.get(b["parent_hash"])
            b["parent"] = p if p is not None and p != i else None
        self.stats["esqueletos"] += 1
        return bones

    def _instance(self, c):
        node = {"name": None, "def": None, "matrix": None, "children": []}
        for k in sanchunks.children(self.buf, c):
            if k.tag == "GINH":
                b = self.p(k)
                xb = struct.unpack_from("<3f", b, 2)
                zb = struct.unpack_from("<3f", b, 14)
                pos = struct.unpack_from("<3f", b, 26)
                scl = struct.unpack_from("<3f", b, 38)
                node["name"] = cstr(b[62:94])
                node["def"] = struct.unpack_from("<I", b, 94)[0]
                node["matrix"] = basis_matrix(xb, zb, pos, scl)
                self.stats["instances"] += 1
            elif k.tag == "GINS":
                node["children"].append(self._instance(k))
        return node


# ---------------------------------------------------------------- glTF writer

class Gltf:
    def __init__(self):
        self.bin = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "sly4 san2gltf.py"}, "scene": 0, "scenes": [{"nodes": []}],
                    "nodes": [], "meshes": [], "materials": [], "textures": [], "images": [], "samplers": [],
                    "buffers": [], "bufferViews": [], "accessors": []}

    def _view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.bin += data
        self.doc["bufferViews"].append(view)
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, values, kind, comp, normalized=False, target=34962, minmax=False):
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[kind]
        fmt = {5126: "f", 5121: "B", 5123: "H", 5125: "I"}[comp]
        flat = values if n == 1 else [x for v in values for x in v]
        acc = {"bufferView": self._view(struct.pack(f"<{len(flat)}{fmt}", *flat), target), "componentType": comp,
               "count": len(values), "type": kind}
        if normalized:
            acc["normalized"] = True
        if minmax:
            if n == 1:
                acc["min"], acc["max"] = [min(values)], [max(values)]
            else:
                acc["min"] = [min(v[i] for v in values) for i in range(n)]
                acc["max"] = [max(v[i] for v in values) for i in range(n)]
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1

    def add(self, key, obj):
        self.doc[key].append(obj)
        return len(self.doc[key]) - 1

    def save(self, path):
        base = os.path.splitext(os.path.basename(path))[0]
        with open(os.path.join(os.path.dirname(path), base + ".bin"), "wb") as f:
            f.write(self.bin)
        self.doc["buffers"] = [{"uri": base + ".bin", "byteLength": len(self.bin)}]
        for k in [k for k, v in self.doc.items() if v == []]:
            del self.doc[k]
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.doc, f, separators=(",", ":"))


def safe(name):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("._") or "x"


def export(scene, out_dir, base, mode, write_textures, write_obj, vertex_colors=False, only_mesh=None):
    os.makedirs(out_dir, exist_ok=True)
    g = Gltf()
    tex_dir = os.path.join(out_dir, "textures")
    image_of, material_of, mesh_of = {}, {}, {}
    cutout = set()  # texture indices whose alpha has transparent texels

    def image(tex_hash):
        if tex_hash in image_of:
            return image_of[tex_hash]
        image_of[tex_hash] = None
        if not write_textures or tex_hash not in scene.textures:
            return None
        name, t = scene.textures[tex_hash]
        if t.base not in texr2dds.FORMATS or t.dim == 3:
            return None
        try:
            from PIL import Image
            dds = texr2dds.dds_header(t) + texr2dds.to_dds_payload(t)
            os.makedirs(tex_dir, exist_ok=True)
            fn = safe(os.path.splitext(name)[0]) + ".png"
            with Image.open(io.BytesIO(dds)) as im:
                im.save(os.path.join(tex_dir, fn))
                has_cutout = im.mode == "RGBA" and im.getchannel("A").getextrema()[0] < 128
        except Exception as e:  # keep exporting geometry if one texture fails
            scene.stats[f"textura_falhou: {type(e).__name__}"] += 1
            return None
        if not g.doc["samplers"]:
            g.add("samplers", {"wrapS": 10497, "wrapT": 10497})
        img = g.add("images", {"uri": "textures/" + fn, "name": name})
        image_of[tex_hash] = g.add("textures", {"source": img, "sampler": 0})
        if has_cutout:
            cutout.add(image_of[tex_hash])
        return image_of[tex_hash]

    def material(mat_hash):
        if mat_hash in material_of:
            return material_of[mat_hash]
        m = scene.materials.get(mat_hash)
        mat = {"name": m["name"] if m else f"mat_{mat_hash:08X}",
               "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 1.0}}
        if m:
            tex = image(m["diffuse"]) if m["diffuse"] else None
            if tex is not None:
                mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": tex}
            if m["blend"] != 0:
                mat["alphaMode"] = "BLEND"  # 3 is additive in game; glTF has no additive mode
            elif tex in cutout:
                mat["alphaMode"], mat["alphaCutoff"] = "MASK", 0.5
            if m["cull"] == 2:
                mat["doubleSided"] = True
            mat["extras"] = {"blendMode": m["blend"], "cullMode": m["cull"], "flags": m["flags"],
                             "additive": m["blend"] == 3}
        material_of[mat_hash] = g.add("materials", mat)
        return material_of[mat_hash]

    def mesh(mesh_hash):
        if mesh_hash in mesh_of:
            return mesh_of[mesh_hash]
        prims = []
        for sub in scene.meshes.get(mesh_hash, []):
            attrs = {"POSITION": g.accessor(sub["pos"], "VEC3", 5126, minmax=True)}
            if sub["normal"]:
                normals = []
                for n in sub["normal"]:
                    ln = math.sqrt(sum(x * x for x in n)) or 1.0
                    normals.append(tuple(x / ln for x in n))
                attrs["NORMAL"] = g.accessor(normals, "VEC3", 5126)
            if sub["uv"]:
                attrs["TEXCOORD_0"] = g.accessor([tuple(v[:2]) for v in sub["uv"]], "VEC2", 5126)
            if sub["color"] and len(sub["color"][0]) == 4:
                key = "COLOR_0" if vertex_colors else "_COLOR_RAW"
                attrs[key] = g.accessor(sub["color"], "VEC4", 5121, normalized=True)
            if mesh_hash in skinned:
                # every primitive of a skinned mesh needs joints; unweighted ones follow the root bone
                nb = len(skinned[mesh_hash])
                src_j = sub.get("joints") or [(0, 0, 0, 0)] * len(sub["pos"])
                src_w = sub.get("weights") or [(255, 0, 0, 0)] * len(sub["pos"])
                joints, weights = [], []
                for j, w in zip(src_j, src_w):
                    j = tuple(x if x < nb else 0 for x in j)
                    if not any(w):
                        j, w = (0, 0, 0, 0), (255, 0, 0, 0)
                    joints.append(j)
                    weights.append(w)
                attrs["JOINTS_0"] = g.accessor(joints, "VEC4", 5123)
                attrs["WEIGHTS_0"] = g.accessor(weights, "VEC4", 5121, normalized=True)
            idx = g.accessor(sub["indices"], "SCALAR", 5123 if len(sub["pos"]) < 65536 else 5125, target=34963)
            prims.append({"attributes": attrs, "indices": idx, "material": material(sub["material"])})
        if not prims:
            mesh_of[mesh_hash] = None
            return None
        mesh_of[mesh_hash] = g.add("meshes", {"name": scene.mesh_names.get(mesh_hash, f"mesh_{mesh_hash:08X}"),
                                              "primitives": prims})
        return mesh_of[mesh_hash]

    # mesh hash -> skeleton, for meshes with skin weights whose geometry carries a SKEL
    skinned = {}
    for _name, mesh_hash, skeleton in scene.geos.values():
        if skeleton and any(sub.get("joints") for sub in scene.meshes.get(mesh_hash, [])):
            skinned.setdefault(mesh_hash, skeleton)
    inverse_binds = {}

    def skin(mesh_hash):
        """New joint nodes (local bind-pose transforms) + glTF skin; returns (root joints, skin)."""
        bones = skinned[mesh_hash]
        if mesh_hash not in inverse_binds:
            inverse_binds[mesh_hash] = g.accessor([mat_inverse(b["world"]) for b in bones], "MAT4", 5126, target=None)
        first = len(g.doc["nodes"])
        for i, b in enumerate(bones):
            local = b["world"] if b["parent"] is None else mat_mul(mat_inverse(bones[b["parent"]]["world"]), b["world"])
            g.add("nodes", {"name": b["name"], "matrix": [round(x, 7) for x in local]})
        roots = []
        for i, b in enumerate(bones):
            if b["parent"] is None:
                roots.append(first + i)
            else:
                g.doc["nodes"][first + b["parent"]].setdefault("children", []).append(first + i)
        sk = g.add("skins", {"joints": list(range(first, first + len(bones))), "skeleton": roots[0],
                             "inverseBindMatrices": inverse_binds[mesh_hash]})
        return roots, sk

    def attach(n, mesh_hash):
        m = mesh(mesh_hash)
        if m is None:
            return
        n["mesh"] = m
        if mesh_hash in skinned:
            roots, sk = skin(mesh_hash)
            n["skin"] = sk
            n.setdefault("children", []).extend(roots)
            scene.stats["malhas_com_esqueleto"] += 1

    def node(inst):
        n = {"name": inst["name"] or "instance"}
        if inst["matrix"] and inst["matrix"] != [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]:
            n["matrix"] = inst["matrix"]
        geo = scene.geos.get(inst["def"])
        if geo and geo[1] is not None:
            attach(n, geo[1])
        kids = [node(c) for c in inst["children"]]
        if kids:
            n.setdefault("children", []).extend(kids)
        return g.add("nodes", n)

    def animate(sk, bones):
        """One glTF animation per Havok clip that fits this skeleton, sampled every frame."""
        joints = g.doc["skins"][sk]["joints"]
        anims = []
        for clip_name, anim, track_map in scene.clips_for(bones):
            frames = [anim.sample(f) for f in range(anim.num_frames)]
            times = [round(f * anim.frame_duration, 6) for f in range(anim.num_frames)]
            t_all = g.accessor(times, "SCALAR", 5126, target=None, minmax=True)
            t_one = None
            samplers, channels = [], []
            for t, j in enumerate(track_map):
                if j is None:
                    continue
                rots, prev = [], None
                for fr in frames:  # keep quaternion signs continuous for interpolation
                    q = fr[t][1]
                    if prev is not None and sum(a * b for a, b in zip(q, prev)) < 0:
                        q = tuple(-x for x in q)
                    rots.append(q)
                    prev = q
                for path, values, kind in (("translation", [fr[t][0] for fr in frames], "VEC3"),
                                           ("rotation", rots, "VEC4"),
                                           ("scale", [fr[t][2] for fr in frames], "VEC3")):
                    constant = all(max(abs(a - b) for a, b in zip(v, values[0])) < 1e-5 for v in values)
                    if constant and path == "scale" and max(abs(x - 1) for x in values[0]) < 1e-5:
                        continue
                    if constant:
                        if t_one is None:
                            t_one = g.accessor([0.0], "SCALAR", 5126, target=None, minmax=True)
                        inp, out = t_one, g.accessor([values[0]], kind, 5126, target=None)
                    else:
                        inp, out = t_all, g.accessor(values, kind, 5126, target=None)
                    samplers.append({"input": inp, "output": out, "interpolation": "LINEAR"})
                    channels.append({"sampler": len(samplers) - 1, "target": {"node": joints[j], "path": path}})
            if channels:
                anims.append(g.add("animations", {"name": clip_name, "samplers": samplers, "channels": channels}))
                scene.stats["animacoes"] += 1
        return anims

    g.doc.setdefault("skins", [])
    g.doc.setdefault("animations", [])
    if mode == "scene":
        roots = [node(r) for r in scene.roots]
    elif mode == "character":
        n = {"name": scene.mesh_names.get(only_mesh, f"mesh_{only_mesh:08X}")}
        attach(n, only_mesh)
        roots = [g.add("nodes", n)] if "mesh" in n else []
        if "skin" in n:
            animate(n["skin"], skinned[only_mesh])
    else:
        roots = []
        for h in scene.meshes:
            n = {"name": scene.mesh_names.get(h, f"mesh_{h:08X}")}
            attach(n, h)
            if "mesh" in n:
                roots.append(g.add("nodes", n))
    g.doc["scenes"][0]["nodes"] = roots
    path = os.path.join(out_dir, base + ".gltf")
    g.save(path)

    if write_obj:
        write_objs(scene, os.path.join(out_dir, "obj"), image_of, g)
    return path, len(g.doc.get("meshes", [])), len(g.doc.get("nodes", []))


def write_objs(scene, out_dir, image_of, g):
    """One OBJ + MTL per mesh, in model space."""
    os.makedirs(out_dir, exist_ok=True)
    for mesh_hash, subs in scene.meshes.items():
        if not subs:
            continue
        name = safe(scene.mesh_names.get(mesh_hash, f"mesh_{mesh_hash:08X}"))
        lines, mtl, base = [f"mtllib {name}.mtl"], [], 1
        for i, sub in enumerate(subs):
            m = scene.materials.get(sub["material"])
            mname = safe(m["name"]) if m else f"mat_{sub['material']:08X}"
            mtl.append(f"newmtl {mname}")
            tex = image_of.get(m["diffuse"]) if m else None
            if tex is not None:
                mtl.append(f"map_Kd ../{g.doc['images'][g.doc['textures'][tex]['source']]['uri']}")
            lines.append(f"o {name}_{i}")
            lines += [f"v {x:.6f} {y:.6f} {z:.6f}" for x, y, z in sub["pos"]]
            if sub["uv"]:
                lines += [f"vt {u:.6f} {1 - v:.6f}" for u, v in (t[:2] for t in sub["uv"])]
            if sub["normal"]:
                lines += [f"vn {x:.5f} {y:.5f} {z:.5f}" for x, y, z in sub["normal"]]
            lines.append(f"usemtl {mname}")
            ix = sub["indices"]
            for t in range(0, len(ix) - 2, 3):
                a, b, c = (ix[t] + base, ix[t + 1] + base, ix[t + 2] + base)
                if sub["uv"] and sub["normal"]:
                    lines.append(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}")
                elif sub["uv"]:
                    lines.append(f"f {a}/{a} {b}/{b} {c}/{c}")
                else:
                    lines.append(f"f {a} {b} {c}")
            base += len(sub["pos"])
        with open(os.path.join(out_dir, name + ".obj"), "w") as f:
            f.write("\n".join(lines) + "\n")
        with open(os.path.join(out_dir, name + ".mtl"), "w") as f:
            f.write("\n".join(mtl) + "\n")


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
    ap.add_argument("--out", default="/work/models")
    ap.add_argument("--mode", choices=["scene", "meshes", "characters"], default="scene",
                    help="characters: one glTF per skinned mesh with its Havok animation clips")
    ap.add_argument("--obj", action="store_true", help="also write one OBJ per mesh (model space)")
    ap.add_argument("--no-textures", action="store_true")
    ap.add_argument("--vertex-colors", action="store_true", help="write vertex colors as COLOR_0 (tints the model)")
    args = ap.parse_args()

    for path in collect_inputs(args.inputs):
        scene = Scene(path)
        if not scene.meshes:
            continue
        parts = os.path.normpath(path).split(os.sep)
        level = "_".join(safe(p) for p in parts[-3:-1])  # e.g. Paris_Hideout
        base = safe(os.path.basename(path).split(".")[0])
        out_dir = os.path.join(args.out, f"{level}_{base}")
        if args.mode == "characters":
            seen = set()
            for name, mesh_hash, skeleton in scene.geos.values():
                if not skeleton or mesh_hash in seen or not any(s.get("joints") for s in scene.meshes.get(mesh_hash, [])):
                    continue
                seen.add(mesh_hash)
                before = scene.stats["animacoes"]
                path, _m, _n = export(scene, os.path.join(out_dir, "characters"), safe(name), "character",
                                      not args.no_textures, False, args.vertex_colors, only_mesh=mesh_hash)
                print(f"{path}: {scene.stats['animacoes'] - before} animacoes", file=sys.stderr)
            skipped = {k: v for k, v in scene.stats.items() if k.startswith("havok_")}
            if skipped:
                print("   " + ", ".join(f"{k}: {v}" for k, v in sorted(skipped.items())), file=sys.stderr)
            continue
        gltf, nmesh, nnode = export(scene, out_dir, base, args.mode, not args.no_textures, args.obj,
                                    args.vertex_colors)
        s = scene.stats
        print(f"{gltf}: {nmesh} malhas, {nnode} nos, {s['submeshes']} submalhas, {s['vertices']:,} vertices, "
              f"{s['triangles']:,} triangulos, {len(scene.textures)} texturas", file=sys.stderr)
        extra = {k: v for k, v in s.items() if k not in ("submeshes", "vertices", "triangles", "instances")}
        if extra:
            print("   " + ", ".join(f"{k}: {v}" for k, v in sorted(extra.items())), file=sys.stderr)


if __name__ == "__main__":
    main()
