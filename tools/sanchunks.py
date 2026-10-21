"""Walker for Sanzaru chunk containers (.san / .san.cooked).

Every chunk is: tag (4 ASCII bytes) + size (u32 little-endian) + payload,
where size counts the size field itself, so the chunk occupies 4 + size bytes
and the payload is size - 4 bytes. A file is one 'FORM' chunk whose payload is
a list of chunks (PLAT, PROF, TEXR...). A chunk is a container when its payload
parses as a list of chunks that ends exactly at the payload end.
"""
import re
import struct

TAG_RE = re.compile(rb"^[A-Z0-9_]{4}$")


class Chunk:
    __slots__ = ("tag", "start", "data_start", "end", "children", "path")

    def __init__(self, tag, start, data_start, end, path):
        self.tag, self.start, self.data_start, self.end, self.path = tag, start, data_start, end, path
        self.children = None

    def payload(self, buf):
        return buf[self.data_start:self.end]


def _header(buf, o, end):
    if o + 8 > end or not TAG_RE.match(buf[o:o + 4]):
        return None
    size = struct.unpack_from("<I", buf, o + 4)[0]
    if size < 4 or o + 4 + size > end:
        return None
    return buf[o:o + 4].decode("ascii"), o + 4 + size


def _parse_list(buf, start, end):
    """Returns [(tag, start, end)] if [start, end) is exactly a chunk list, else None."""
    out, o = [], start
    while o < end:
        h = _header(buf, o, end)
        if h is None:
            return None
        out.append((h[0], o, h[1]))
        o = h[1]
    return out if out else None


def walk(buf, start=0, end=None, path="", max_depth=8):
    """Yields every chunk (depth-first) in buf[start:end]."""
    end = len(buf) if end is None else end
    items = _parse_list(buf, start, end)
    if items is None:
        return
    for tag, s, e in items:
        c = Chunk(tag, s, s + 8, e, f"{path}/{tag}")
        yield c
        if max_depth > 0 and _parse_list(buf, c.data_start, e) is not None:
            yield from walk(buf, c.data_start, e, c.path, max_depth - 1)


def children(buf, chunk):
    items = _parse_list(buf, chunk.data_start, chunk.end) or []
    return [Chunk(t, s, s + 8, e, f"{chunk.path}/{t}") for t, s, e in items]
