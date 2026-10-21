"""Static map of the decrypted PS3 executable (EBOOT.elf, PPC64 big-endian).

Produces in --out:
  summary.txt     sections, TOC, counts
  functions.csv   function entry points from .opd (+ bl targets)
  imports.csv     imported library functions (module, NID, stub address)
  syscalls.csv    every 'sc' instruction with the LV2 syscall number in r11
  tag_xrefs.md    references to engine chunk tags (TEXR, MESH, ...) with disassembly
  strings.txt     printable strings of the data sections (feed to xmlb2json --names)

Tag references are found three ways:
  * the 4CC built as an immediate: lis rX,HI + ori/addi rY,rX,LO (both byte orders)
  * the address of a string containing the tag: lis/addi absolute, r2 (TOC)
    relative addi, or a TOC slot (lwz/ld d(r2)) holding that address
Usage:
  eboot_scan.py [--elf /game/USRDIR/EBOOT.elf] [--out /work/eboot]
                [--tags TEXR,MESH,...] [--syscall-names lv2.cpp]
"""
import argparse
import bisect
import collections
import csv
import hashlib
import os
import re
import struct
import sys

try:
    import capstone
except ImportError:  # disassembly context is optional
    capstone = None

SHF_EXECINSTR = 0x4
SHF_ALLOC = 0x2
PT_LOAD = 1
PT_PRX_PARAM = 0x60000002
SC = 0x44000002
DEFAULT_TAGS = "TEXR,MESH,MATL,SBKF,GINS,TGTF,TXRH,SMSH,HKAF,HVKF,EMIT,EINS,ENTF,DIRF,PROF,PLAT,FORM,XMLB,SSTR"


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


class Elf:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.b = f.read()
        b = self.b
        if b[:4] != b"\x7fELF" or b[4] != 2 or b[5] != 2:
            raise ValueError("expected a 64-bit big-endian ELF (decrypt the SELF first)")
        (self.entry, phoff, shoff) = struct.unpack_from(">QQQ", b, 0x18)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from(">HHHHH", b, 0x36)
        self.segments = []
        self.libstub = None
        for i in range(phnum):
            p_type, flags, off, vaddr, _pa, filesz, memsz, _al = struct.unpack_from(">IIQQQQQQ", b, phoff + i * phentsize)
            if p_type == PT_LOAD and filesz:
                self.segments.append((vaddr, off, filesz, flags))
            elif p_type == PT_PRX_PARAM and filesz >= 0x20:
                # sys_process_prx_param: size, magic 0x1B434CEC, version, unk,
                # libent_start, libent_end, libstub_start, libstub_end
                magic, = struct.unpack_from(">I", b, off + 4)
                if magic == 0x1B434CEC:
                    self.libstub = struct.unpack_from(">II", b, off + 0x18)
        self.sections = []
        raw = [struct.unpack_from(">IIQQQQIIQQ", b, shoff + i * shentsize) for i in range(shnum)] if shoff else []
        strtab = raw[shstrndx] if raw and shstrndx < len(raw) else None
        for name_off, s_type, flags, addr, off, size, *_ in raw:
            name = ""
            if strtab:
                s = strtab[4] + name_off
                name = b[s:b.index(b"\0", s)].decode("ascii", "replace")
            self.sections.append({"name": name, "type": s_type, "flags": flags, "addr": addr, "off": off, "size": size})
        self._infer_names()

    def _infer_names(self):
        """RPCS3's decrypted ELF has no section name table (it lives outside the
        segments in the SELF), so the important sections are recognised by content."""
        def at(addr):
            return next((s for s in self.sections if s["size"] and s["addr"] <= addr < s["addr"] + s["size"]), None)
        guesses = {}
        opd = at(self.entry)
        if opd and not opd["flags"] & SHF_EXECINSTR:
            guesses[".opd"] = opd
        if self.libstub:
            s = at(self.libstub[0])
            if s:
                guesses[".lib.stub"] = s
        for name, s in guesses.items():
            if not self.section(name) and not s["name"]:
                s["name"] = name
                s["inferred"] = True

    def read(self, vaddr, n):
        for va, off, size, _fl in self.segments:
            if va <= vaddr and vaddr + n <= va + size:
                return self.b[off + vaddr - va: off + vaddr - va + n]
        return None

    def u32(self, vaddr):
        d = self.read(vaddr, 4)
        return struct.unpack(">I", d)[0] if d else None

    def exec_sections(self):
        return [s for s in self.sections if s["flags"] & SHF_EXECINSTR and s["size"] and s["type"] != 8]

    def data_sections(self):
        return [s for s in self.sections if s["flags"] & SHF_ALLOC and not s["flags"] & SHF_EXECINSTR and s["type"] == 1]

    def section(self, name):
        return next((s for s in self.sections if s["name"] == name), None)

    def words(self, sec):
        data = self.b[sec["off"]:sec["off"] + sec["size"]]
        for i in range(0, len(data) - 3, 4):
            yield sec["addr"] + i, struct.unpack_from(">I", data, i)[0]


def find_functions(elf):
    funcs, tocs = set(), collections.Counter()
    opd = elf.section(".opd")
    if opd:
        data = elf.b[opd["off"]:opd["off"] + opd["size"]]
        for i in range(0, len(data) - 7, 8):
            addr, toc = struct.unpack_from(">II", data, i)
            if addr:
                funcs.add(addr)
                tocs[toc] += 1
    else:  # fall back to the entry descriptor
        addr, toc = elf.u32(elf.entry), elf.u32(elf.entry + 4)
        if addr:
            funcs.add(addr)
            tocs[toc] += 1
    for sec in elf.exec_sections():
        for a, w in elf.words(sec):
            if w >> 26 == 18 and (w & 3) == 1:  # bl
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                funcs.add((a + li) & 0xFFFFFFFF)
    return sorted(funcs), tocs


def find_imports(elf):
    out = []
    stub = elf.section(".lib.stub")
    if not stub:
        return out
    size = stub["size"]
    if elf.libstub and elf.libstub[0] == stub["addr"]:
        size = elf.libstub[1] - elf.libstub[0]
    data = elf.b[stub["off"]:stub["off"] + size]
    o = 0
    while o + 0x2C <= len(data):
        size = data[o]
        if size < 0x2C:
            break
        num_func = struct.unpack_from(">H", data, o + 6)[0]
        mod_ptr, nid_tab, func_tab = struct.unpack_from(">III", data, o + 0x10)
        name = elf.read(mod_ptr, 64) or b""
        name = name.split(b"\0", 1)[0].decode("ascii", "replace")
        for i in range(num_func):
            out.append({"module": name, "nid": elf.u32(nid_tab + 4 * i), "slot": func_tab + 4 * i})
        o += size
    # map stub code (li/oris/lwz r12 -> slot) to the import
    by_slot = {imp["slot"]: imp for imp in out}
    for sec in elf.exec_sections():
        hi = None
        for a, w in elf.words(sec):
            op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            if op in (15, 25) and rd == 12:  # lis/oris r12
                hi = (w & 0xFFFF) << 16
                start = a - 4 if op == 25 else a
            elif op == 32 and rd == 12 and ra == 12 and hi is not None:
                slot = (hi + sext16(w & 0xFFFF)) & 0xFFFFFFFF
                if slot in by_slot:
                    by_slot[slot]["stub"] = start
                hi = None
    return out


NID_SUFFIX = bytes.fromhex("6759659904250490566427499489741A")


def nid_of(name):
    """PS3 function NID: first 4 bytes (little-endian) of SHA-1(name + suffix)."""
    return struct.unpack("<I", hashlib.sha1(name.encode() + NID_SUFFIX).digest()[:4])[0]


def nid_names(path):
    names = {}
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                if not line.startswith("#"):
                    for name in line.split():
                        names.setdefault(nid_of(name), name)
    return names


def lv2_names(path):
    names = {}
    if not path:
        return names
    pat = re.compile(r"\((\w+)\)[^/\n]*//\s*(\d+)")
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            for name, num in pat.findall(line):
                names.setdefault(int(num), name)
            m = re.match(r"\s*(\d+)\s+(\w+)\s*$", line)
            if m:
                names.setdefault(int(m.group(1)), m.group(2))
    return names


def find_syscalls(elf):
    out = []
    for sec in elf.exec_sections():
        words = list(elf.words(sec))
        for i, (a, w) in enumerate(words):
            if w != SC:
                continue
            num = None
            for j in range(i - 1, max(-1, i - 24), -1):
                pw = words[j][1]
                op, rd, ra = pw >> 26, (pw >> 21) & 31, (pw >> 16) & 31
                if op == 14 and rd == 11 and ra == 0:  # li r11, N
                    num = sext16(pw & 0xFFFF)
                    break
                if pw == 0x4E800020 or pw >> 26 == 18:  # blr / b: left the block
                    break
            out.append((a, num))
    return out


def find_constant_compares(words, values, hits):
    """Compiler idiom for 'x == CONST': addis rD,rX,-HI (or xoris rD,rX,HI) ; cmplwi rD,LO."""
    for i, (a, w) in enumerate(words):
        op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15 and ra != 0:
            hi = (-sext16(imm)) & 0xFFFF
        elif op == 27:  # xoris rA,rS,HI writes rA (bits 16-20)
            hi, rd = imm, ra
        else:
            continue
        for k in range(i + 1, min(len(words), i + 6)):
            nw = words[k][1]
            if nw >> 26 in (10, 11) and (nw >> 16) & 31 == rd:  # cmplwi / cmpwi
                lo = nw & 0xFFFF
                val = ((hi << 16) + (lo if nw >> 26 == 10 else sext16(lo))) & 0xFFFFFFFF
                if val in values:
                    hits[val].append((a, words[k][0], "cmp"))
                break


def find_constant_builds(elf, values):
    """lis rX,HI ; ... ; ori|addi rY,rX,LO  ->  value (plus compare idioms)."""
    hits = collections.defaultdict(list)
    for sec in elf.exec_sections():
        words = list(elf.words(sec))
        find_constant_compares(words, values, hits)
        for i, (a, w) in enumerate(words):
            if w >> 26 != 15 or (w >> 16) & 31 != 0:  # lis = addis rD,0,imm
                continue
            rx, hi = (w >> 21) & 31, (w & 0xFFFF) << 16
            for k in range(i + 1, min(len(words), i + 12)):
                nw = words[k][1]
                op, imm = nw >> 26, nw & 0xFFFF
                if op in (14, 24) and src_reg(nw) == rx:
                    val = (hi | imm) if op == 24 else (hi + sext16(imm)) & 0xFFFFFFFF
                    if val in values:
                        hits[val].append((a, words[k][0], "imm"))
                    break
                if op == 15 and (nw >> 21) & 31 == rx:
                    break
    return hits


def src_reg(w):
    """Base/source register of addi (14), lwz (32), ld (58) = rA; of ori (24) = rS."""
    return (w >> 21) & 31 if w >> 26 == 24 else (w >> 16) & 31


def find_address_refs(elf, targets, tocs):
    """References to any address in targets (absolute lis/addi, TOC-relative, TOC slot)."""
    hits = collections.defaultdict(list)
    toc_values = list(tocs)
    for sec in elf.exec_sections():
        words = list(elf.words(sec))
        for i, (a, w) in enumerate(words):
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if ra == 2 and op in (14, 32, 58):
                d = sext16(imm) & (~3 if op == 58 else ~0)
                for toc in toc_values:
                    ea = (toc + d) & 0xFFFFFFFF
                    if op == 14 and ea in targets:
                        hits[ea].append((a, a, "toc+d"))
                    elif op in (32, 58):
                        ptr = elf.u32(ea + (4 if op == 58 else 0))
                        if ptr in targets:
                            hits[ptr].append((a, a, f"toc slot 0x{ea:X}"))
                        elif ptr is not None:
                            # anchor: rX = *(r2+d) is a section base, then addi rY, rX, off
                            for k in range(i + 1, min(len(words), i + 10)):
                                nw = words[k][1]
                                if nw >> 26 == 14 and src_reg(nw) == rd:
                                    ref = (ptr + sext16(nw & 0xFFFF)) & 0xFFFFFFFF
                                    if ref in targets:
                                        hits[ref].append((a, words[k][0], f"ancora toc 0x{ea:X}+0x{sext16(nw & 0xFFFF):X}"))
                                    break
                                if (nw >> 21) & 31 == rd and nw >> 26 in (14, 15, 24, 32, 58):
                                    break
            if op == 15 and (ra == 0 or ra == 2):
                base = (imm << 16) if ra == 0 else None
                for k in range(i + 1, min(len(words), i + 12)):
                    nw = words[k][1]
                    nop, nrd, nimm = nw >> 26, (nw >> 21) & 31, nw & 0xFFFF
                    if nop in (14, 24, 32) and src_reg(nw) == rd:
                        bases = [base] if base is not None else [(t + (sext16(imm) << 16)) & 0xFFFFFFFF for t in toc_values]
                        for bv in bases:
                            ea = (bv | nimm) if nop == 24 else (bv + sext16(nimm)) & 0xFFFFFFFF
                            if nop == 32:
                                ptr = elf.u32(ea)
                                if ptr in targets:
                                    hits[ptr].append((a, words[k][0], f"slot 0x{ea:X}"))
                            elif ea in targets:
                                hits[ea].append((a, words[k][0], "abs" if base is not None else "toc hi/lo"))
                        break
                    if nop == 15 and nrd == rd:
                        break
    return hits


def find_strings(elf, min_len=4):
    out = []
    for sec in elf.data_sections():
        data = elf.b[sec["off"]:sec["off"] + sec["size"]]
        for m in re.finditer(rb"[\x20-\x7e]{%d,}" % min_len, data):
            out.append((sec["addr"] + m.start(), m.group().decode("ascii")))
    return out


def containing(funcs, addr):
    i = bisect.bisect_right(funcs, addr) - 1
    return funcs[i] if i >= 0 else None


def disasm(elf, addr, before=6, after=6):
    start = addr - 4 * before
    code = elf.read(start, 4 * (before + after + 1)) or b""
    md = None
    if capstone is not None:
        md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_64 | capstone.CS_MODE_BIG_ENDIAN)
    lines = []
    for i in range(0, len(code) - 3, 4):
        a, word = start + i, code[i:i + 4]
        ins = next(md.disasm(word, a), None) if md else None
        text = f"{ins.mnemonic:<8} {ins.op_str}" if ins else f".long 0x{word.hex()}"
        lines.append(f"{'=>' if a == addr else '  '} {a:08X}: {text}")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", default="/game/USRDIR/EBOOT.elf")
    ap.add_argument("--out", default="/work/eboot")
    ap.add_argument("--tags", default=DEFAULT_TAGS)
    ap.add_argument("--syscall-names", help="RPCS3 lv2.cpp or a 'number name' text file")
    ap.add_argument("--nid-names", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "ps3_nid_candidates.txt"),
                    help="candidate function names; a name is used only if its NID matches")
    args = ap.parse_args()
    if not os.path.exists(args.elf):
        sys.exit(f"{args.elf} nao encontrado. Gere o EBOOT.elf no RPCS3 (Utilities > Decrypt PS3 Binaries) "
                 "e coloque em PS3_GAME/USRDIR, ou informe o caminho com --elf.")
    os.makedirs(args.out, exist_ok=True)

    elf = Elf(args.elf)
    funcs, tocs = find_functions(elf)
    imports = find_imports(elf)
    syscalls = find_syscalls(elf)
    sysnames = lv2_names(args.syscall_names)
    strings = find_strings(elf)

    with open(os.path.join(args.out, "summary.txt"), "w") as f:
        f.write(f"entry descriptor: 0x{elf.entry:X}\n")
        f.write("TOC (r2) values: " + ", ".join(f"0x{t:X} x{c}" for t, c in tocs.most_common()) + "\n\n")
        f.write(f"{'section':<24}{'addr':>10}{'size':>10}  flags\n")
        for s in elf.sections:
            if s["size"]:
                name = s["name"] + (" (inferido)" if s.get("inferred") else "")
                f.write(f"{name:<24}{s['addr']:>10X}{s['size']:>10X}  {s['flags']:X}\n")
        f.write(f"\nfunctions: {len(funcs)}\nimports: {len(imports)} ({len({i['module'] for i in imports})} modules)\n")
        f.write(f"sc instructions: {len(syscalls)}\nstrings: {len(strings)}\n")

    with open(os.path.join(args.out, "functions.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["address"])
        w.writerows([[f"0x{a:08X}"] for a in funcs])

    nids = nid_names(args.nid_names)
    with open(os.path.join(args.out, "imports.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["module", "nid", "name", "slot", "stub"])
        for imp in imports:
            w.writerow([imp["module"], f"0x{imp['nid']:08X}" if imp["nid"] is not None else "", nids.get(imp["nid"], ""),
                        f"0x{imp['slot']:08X}", f"0x{imp['stub']:08X}" if "stub" in imp else ""])
    named = sum(1 for imp in imports if imp["nid"] in nids)

    with open(os.path.join(args.out, "syscalls.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["address", "function", "syscall", "name"])
        for a, num in syscalls:
            fn = containing(funcs, a)
            w.writerow([f"0x{a:08X}", f"0x{fn:08X}" if fn is not None else "", "" if num is None else num,
                        sysnames.get(num, "")])

    with open(os.path.join(args.out, "strings.txt"), "w", encoding="utf-8") as f:
        f.writelines(f"{s}\n" for _a, s in strings)

    # ---- tag cross references ----
    tags = [t.strip() for t in args.tags.split(",") if len(t.strip()) == 4]
    imm_values = {}
    for t in tags:
        be = struct.unpack(">I", t.encode())[0]
        imm_values[be] = (t, "BE")
        imm_values[struct.unpack("<I", t.encode())[0]] = (t, "LE")
    imm_hits = find_constant_builds(elf, set(imm_values))

    string_targets = {}  # address -> (tag, string)
    for addr, s in strings:
        for t in tags:
            pos = s.find(t)
            while pos >= 0:
                string_targets[addr + pos] = (t, s)
                string_targets.setdefault(addr, (t, s))
                pos = s.find(t, pos + 1)
    addr_hits = find_address_refs(elf, set(string_targets), tocs)

    # strong = the tag as an immediate or a string that is exactly the tag;
    # weak = the tag inside a longer string (e.g. "hkcdShapeType::COMPRESSED_MESH")
    found = {t: {"strong": [], "weak": []} for t in tags}
    for val, (tag, order) in imm_values.items():
        for _start, use, kind in imm_hits.get(val, []):
            what = "comparacao" if kind == "cmp" else "imediato"
            found[tag]["strong"].append((use, f"{what} {order} 0x{val:08X}"))
    for addr, (tag, s) in sorted(string_targets.items()):
        for _start, use, kind in addr_hits.get(addr, []):
            key = "strong" if s == tag else "weak"
            found[tag][key].append((use, f"string 0x{addr:08X} \"{s[:60]}\" via {kind}"))

    with open(os.path.join(args.out, "tag_xrefs.md"), "w", encoding="utf-8") as f:
        f.write("# Referencias aos blocos do engine\n\n| Tag | Funcoes (referencia direta) |\n|---|---|\n")
        for t in tags:
            fns = sorted({containing(funcs, use) or 0 for use, _ in found[t]["strong"]})
            f.write(f"| {t} | {', '.join(f'0x{x:08X}' for x in fns) or '-'} |\n")
        f.write("\n")
        for t in tags:
            f.write(f"## {t}\n\n")
            for use, what in found[t]["strong"]:
                fn = containing(funcs, use)
                f.write(f"- {what} em 0x{use:08X} (funcao 0x{fn or 0:08X})\n\n```\n{disasm(elf, use)}\n```\n\n")
            if not found[t]["strong"]:
                f.write("_nenhuma referencia direta encontrada_\n\n")
            if found[t]["weak"]:
                f.write("Ocorrencias dentro de strings maiores (provavel ruido):\n\n")
                for use, what in found[t]["weak"]:
                    f.write(f"- {what} em 0x{use:08X} (funcao 0x{containing(funcs, use) or 0:08X})\n")
                f.write("\n")
            print(f"{t}: {len(found[t]['strong'])} diretas, {len(found[t]['weak'])} em strings maiores", file=sys.stderr)

    print(f"funcoes={len(funcs)} imports={len(imports)} (com nome: {named}) sc={len(syscalls)} strings={len(strings)} -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
