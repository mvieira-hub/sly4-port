"""Converts Sanzaru XMLB (.xmb) files to XML or JSON.

Layout (big-endian), reverse-engineered from the game's 761 script files and
confirmed against the loader in EBOOT (0x004FACCC, relocation 0x004FABB4):
  0x00 'XMLB'  u16 version (4)  u16 0
  0x08 'PS3 '  u32 file size
  0x10 u16 root_count  u16 0  u32 root_offsets[root_count]
  element (24 bytes):
    u32 name_hash, u32 text_offset (0 = none; plain NUL-terminated text),
    u32 parent_offset (0 for roots),
    u16 child_count, u16 attr_count, u32 children_offset, u32 attrs_offset
  children: u32[child_count] offsets of child elements
  attrs:    {u32 name_hash, u32 value_offset}[attr_count]
  string:   u32 hash, char[] NUL-terminated, padded to 4 bytes
Names and strings are hashed with h = h*31 + c over the UPPERCASE text
(Java String.hashCode style), so names are recovered from a dictionary.

Attribute values carry no type tag (the engine knows the schema), so they are
decoded heuristically:
  * string      -> hash matches the text that follows it and it fits before
                   the next referenced offset
  * raw         -> bytes up to the next referenced offset, shown as u32 ("u:")
                   or float ("f:") when every word looks like a float, else hex ("x:")
Name dictionary: seed words + every string value in the input files + --names
files, split into tokens; then pairs of short tokens are tried (--combine) to
catch CamelCase names. --names-map writes hash -> name -> source for auditing.
Usage:
  xmlb2json.py INPUT... [--out DIR] [--format xml|json] [--names FILE ...]
"""
import argparse
import bisect
import json
import math
import os
import re
import struct
import sys
from xml.sax.saxutils import escape, quoteattr

ELEM = struct.Struct(">IIIHHII")

SEED_NAMES = """
Name Level Levels File Files Path Type Class Id ID Value Values Data Root Item Items List Entry Entries
Actor Actors Spawn Spawns Goal Goals Job Jobs Episode Episodes Director Script Scripts Event Events
Trigger Triggers Action Actions Condition Conditions State States Param Params Parameter Parameters
Position Pos Rotation Rot Scale Transform Offset Target Targets Time Duration Delay Speed Count Index
Enabled Visible Active Default Min Max Radius Width Height Color Colour Sound Sounds Music Volume
Animation Animations Anim Camera Character Characters Player Players Object Objects Instance Instances
Texture Textures Mesh Material Movie Movies Text Title Description Desc Icon Image Award Awards
Mission Missions Objective Objectives Checkpoint Checkpoints Zone Zones Region Regions Node Nodes
Link Links Child Children Parent Group Groups Layer Layers Tag Tags Key Keys Mode Flags Flag Priority
xmlns xmlns:xsi xmlns:xs xsi:schemaLocation xsi:noNamespaceSchemaLocation targetNamespace
Workbook DocumentProperties Author LastAuthor Created LastSaved Company Version OfficeDocumentSettings
AllowPNG ExcelWorkbook WindowHeight WindowWidth WindowTopX WindowTopY ProtectStructure ProtectWindows
Styles Style Alignment Borders Border Font Interior NumberFormat Protection Worksheet Table Column Row Cell
Data WorksheetOptions PageSetup Header Footer PageMargins Unsynced Print ValidPrinterInfo
HorizontalResolution VerticalResolution Selected Panes Pane Number ActiveRow ActiveCol ProtectObjects
ProtectScenarios FreezePanes FrozenNoSplit SplitHorizontal TopRowBottomPane ActivePane DisplayGridlines
xmlns:o xmlns:x xmlns:ss xmlns:html ss:Name ss:Type ss:ID ss:Index ss:StyleID ss:Width ss:AutoFitWidth
ss:AutoFitHeight ss:ExpandedColumnCount ss:ExpandedRowCount x:FullColumns x:FullRows ss:DefaultRowHeight
ss:DefaultColumnWidth ss:Vertical ss:Horizontal ss:FontName x:Family x:CharSet ss:Size ss:Color ss:Bold
ss:Italic ss:Underline ss:Height ss:Span ss:MergeAcross ss:MergeDown ss:Hidden ss:Format ss:WrapText
ss:Pattern ss:PatternColor ss:Position ss:LineStyle ss:Weight ss:Parent ss:Formula ss:HRef ss:Ticked
""".split()


def x31(text):
    h = 0
    for c in text.upper().encode("latin-1", "replace"):
        h = (h * 31 + c) & 0xFFFFFFFF
    return h


class Names:
    def __init__(self):
        self.by_hash = {}
        self.source = {}
        self.tokens = {}

    def add(self, word, source="dict"):
        word = word.strip()
        if word and len(word) < 128:
            h = x31(word)
            if h not in self.by_hash:
                self.by_hash[h], self.source[h] = word, source

    def add_text(self, text, source="dict"):
        self.add(text, source)
        for tok in re.split(r"[^A-Za-z0-9_]+", text):
            self.add(tok, source)
            for part in tok.split("_"):
                self.add(part, source)
            # CamelCase pieces: "SpawnPoint" -> Spawn, Point, SpawnPoint
            for part in re.findall(r"[A-Z]+[a-z0-9]*|[a-z0-9]+", tok):
                self.add(part, source)
                if 2 <= len(part) <= 12 and part[0].isupper():
                    self.tokens[part] = self.tokens.get(part, 0) + 1

    def combine(self, wanted, top):
        """Tries Word1+Word2 for the most frequent tokens; only fills hashes in wanted."""
        words = [w for w, _ in sorted(self.tokens.items(), key=lambda kv: -kv[1])[:top]]
        hashed = [(w, x31(w), pow(31, len(w), 1 << 32)) for w in words]
        wanted = set(wanted) - set(self.by_hash)
        for a, ha, _ in hashed:
            for b, hb, pb in hashed:
                h = (ha * pb + hb) & 0xFFFFFFFF
                if h in wanted:
                    self.by_hash[h], self.source[h] = a + b, "pair"
                    wanted.discard(h)

    def get(self, h):
        return self.by_hash.get(h)


class Xmlb:
    def __init__(self, buf, path=""):
        self.b, self.path = buf, path
        if buf[:4] != b"XMLB":
            raise ValueError("not XMLB")
        self.version = struct.unpack_from(">H", buf, 4)[0]
        self.platform = buf[8:12].decode("ascii", "replace")
        self.root_count = struct.unpack_from(">H", buf, 0x10)[0]
        self.roots = list(struct.unpack_from(f">{self.root_count}I", buf, 0x14))
        self.elements = {}
        self.refs = set()
        for r in self.roots:
            self._collect(r)
        self.sorted_refs = sorted(self.refs | {len(buf)})

    def _collect(self, o):
        if o in self.elements:
            return
        h, text, parent, n, na, ch, at = ELEM.unpack_from(self.b, o)
        self.elements[o] = (h, text, parent, n, na, ch, at)
        self.refs.update((o, ch, at) if n or na else (o,))
        if text:
            self.refs.add(text)
        for i in range(na):
            _name, v = struct.unpack_from(">II", self.b, at + i * 8)
            self.refs.add(v)
        for i in range(n):
            self._collect(struct.unpack_from(">I", self.b, ch + i * 4)[0])

    def string_at(self, o, limit=None):
        """Returns the string at o if its stored hash matches (and the NUL lies before o + limit)."""
        b = self.b
        if o + 5 > len(b):
            return None
        end = b.find(b"\0", o + 4)
        if end < 0 or (limit is not None and end >= o + limit):
            return None
        raw = b[o + 4:end]
        if struct.unpack_from(">I", b, o)[0] != x31(raw.decode("latin-1")):
            return None
        return raw.decode("utf-8", "replace")

    def extent(self, o):
        """Bytes from o to the next referenced offset."""
        i = bisect.bisect_right(self.sorted_refs, o)
        return (self.sorted_refs[i] if i < len(self.sorted_refs) else len(self.b)) - o

    def text_at(self, o):
        end = self.b.find(b"\0", o)
        return self.b[o:end if end >= 0 else len(self.b)].decode("utf-8", "replace")

    def all_name_hashes(self):
        for h, _text, _parent, _n, na, _ch, at in self.elements.values():
            yield h
            for i in range(na):
                yield struct.unpack_from(">I", self.b, at + i * 8)[0]


def fmt_float(f):
    if math.isfinite(f) and (f == 0 or 1e-6 <= abs(f) <= 1e7):
        return repr(round(f, 6))
    return None


def decode_raw(buf, o, size):
    size = max(0, min(size, 64))
    words = [struct.unpack_from(">I", buf, o + i)[0] for i in range(0, size - size % 4, 4)]
    out = {"hex": buf[o:o + size].hex()}
    if words:
        out["u32"] = words
        floats = [fmt_float(struct.unpack(">f", struct.pack(">I", w))[0]) for w in words]
        if any(w > 0xFFFF for w in words) and all(f is not None for f in floats):
            out["f32"] = [float(f) for f in floats]
    return out


def to_tree(x, names, o, stats):
    h, text, _parent, n, na, ch, at = x.elements[o]
    tag = names.get(h)
    stats["names_total"] += 1
    stats["names_resolved"] += tag is not None
    node = {"tag": tag or f"h_{h:08X}", "attrs": [], "children": []}
    if text:
        node["text"] = x.text_at(text)
        stats["v_text"] += 1
    for i in range(na):
        ah, v = struct.unpack_from(">II", x.b, at + i * 8)
        aname = names.get(ah)
        stats["names_total"] += 1
        stats["names_resolved"] += aname is not None
        aname = aname or f"h_{ah:08X}"
        size = x.extent(v)
        s = x.string_at(v, size)
        if s is not None:
            node["attrs"].append((aname, "string", s))
            stats["v_string"] += 1
            continue
        node["attrs"].append((aname, "raw", decode_raw(x.b, v, size)))
        stats["v_raw"] += 1
    for i in range(n):
        node["children"].append(to_tree(x, names, struct.unpack_from(">I", x.b, ch + i * 4)[0], stats))
    return node


def render_xml(node, depth=0):
    pad = "  " * depth
    attrs = []
    for name, kind, val in node["attrs"]:
        if kind == "string":
            attrs.append(f"{name}={quoteattr(val)}")
        elif "f32" in val:
            attrs.append(f"{name}={quoteattr('f:' + ','.join(map(str, val['f32'])))}")
        elif "u32" in val and len(val["u32"]) <= 4:
            attrs.append(f"{name}={quoteattr('u:' + ','.join(map(str, val['u32'])))}")
        else:
            attrs.append(f"{name}={quoteattr('x:' + val['hex'])}")
    tag = re.sub(r"[^A-Za-z0-9_.:-]", "_", node["tag"])
    head = f"{pad}<{tag}{' ' if attrs else ''}{' '.join(attrs)}"
    text = escape(node.get("text", ""))
    if not node["children"]:
        return f"{head}>{text}</{tag}>" if text else head + "/>"
    body = "\n".join(render_xml(c, depth + 1) for c in node["children"])
    return f"{head}>{text}\n{body}\n{pad}</{tag}>"


def render_json(node):
    out = {"tag": node["tag"]}
    if "text" in node:
        out["text"] = node["text"]
    if node["attrs"]:
        out["attrs"] = {name: val for name, _kind, val in node["attrs"]}
    if node["children"]:
        out["children"] = [render_json(c) for c in node["children"]]
    return out


def collect_inputs(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                for f in sorted(files):
                    if f.lower().endswith(".xmb"):
                        yield os.path.join(root, f)
        else:
            yield p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--out", default="/work/scripts")
    ap.add_argument("--format", choices=["xml", "json"], default="xml")
    ap.add_argument("--names", action="append", default=[], help="text file with candidate names, one per line")
    ap.add_argument("--base", default="/work/psarc", help="input prefix stripped from output paths")
    ap.add_argument("--unknown", help="write unresolved name hashes (with counts) to this file")
    ap.add_argument("--names-map", help="write hash -> name -> source (seed/data/names/pair) to this file")
    ap.add_argument("--combine", type=int, default=1500, help="top-N tokens tried in pairs (0 = off)")
    args = ap.parse_args()

    files = list(collect_inputs(args.inputs))
    names = Names()
    for w in SEED_NAMES:
        names.add(w, "seed")
    for path in args.names:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                names.add_text(line.strip(), "names")
    parsed = []
    for p in files:
        with open(p, "rb") as f:
            x = Xmlb(f.read(), p)
        for o in x.refs:
            s = x.string_at(o, x.extent(o))
            if s:
                names.add_text(s, "data")
        names.add_text(os.path.splitext(os.path.basename(p))[0], "data")
        parsed.append(x)
    if args.combine:
        names.combine({h for x in parsed for h in x.all_name_hashes()}, args.combine)
    if args.names_map:
        with open(args.names_map, "w", encoding="utf-8") as f:
            for h in sorted({h for x in parsed for h in x.all_name_hashes()}):
                f.write(f"{h:08X}\t{names.get(h) or ''}\t{names.source.get(h, '')}\n")

    stats = {k: 0 for k in ("names_total", "names_resolved", "v_string", "v_raw", "v_text")}
    unknown = {}
    for x in parsed:
        roots = [to_tree(x, names, r, stats) for r in x.roots]
        rel = os.path.relpath(x.path, args.base) if x.path.startswith(args.base) else os.path.basename(x.path)
        dest = os.path.join(args.out, os.path.splitext(rel)[0] + "." + args.format)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "w", encoding="utf-8") as f:
            if args.format == "xml":
                f.write('<?xml version="1.0" encoding="utf-8"?>\n')
                f.write("\n".join(render_xml(r) for r in roots) + "\n")
            else:
                json.dump([render_json(r) for r in roots], f, indent=1, ensure_ascii=False)
        if args.unknown:
            def walk(n):
                for t in [n["tag"]] + [a[0] for a in n["attrs"]]:
                    if t.startswith("h_"):
                        unknown[t] = unknown.get(t, 0) + 1
                for c in n["children"]:
                    walk(c)
            for r in roots:
                walk(r)

    if args.unknown:
        with open(args.unknown, "w") as f:
            for k, v in sorted(unknown.items(), key=lambda kv: -kv[1]):
                f.write(f"{k[2:]}\t{v}\n")
    total = stats["names_total"] or 1
    print(f"{len(parsed)} arquivos -> {args.out} ({args.format})", file=sys.stderr)
    print(f"nomes resolvidos: {stats['names_resolved']}/{stats['names_total']} ({100 * stats['names_resolved'] / total:.1f}%)", file=sys.stderr)
    print(f"valores: string={stats['v_string']} bruto={stats['v_raw']} texto_de_elemento={stats['v_text']}", file=sys.stderr)


if __name__ == "__main__":
    main()
