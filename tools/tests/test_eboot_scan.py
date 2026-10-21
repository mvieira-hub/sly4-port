"""Builds a tiny PPC64 big-endian ELF and checks eboot_scan finds what we planted.

Run: .\\tools\\run.ps1 tests/test_eboot_scan.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import eboot_scan  # noqa: E402

BASE = 0x10000


def lis(rd, imm):
    return (15 << 26) | (rd << 21) | (imm & 0xFFFF)


def ori(ra, rs, imm):
    return (24 << 26) | (rs << 21) | (ra << 16) | (imm & 0xFFFF)


def addi(rd, ra, simm):
    return (14 << 26) | (rd << 21) | (ra << 16) | (simm & 0xFFFF)


def lwz(rd, ra, d):
    return (32 << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF)


def build(path):
    text_off, rodata_off = 0x100, 0x200
    text_va, rodata_va = BASE + text_off, BASE + rodata_off
    rodata = b"LoadTEXR\0\0\0\0MESH\0\0\0\0"
    mesh_va = rodata_va + 12
    slot_va = rodata_va + len(rodata)            # TOC slot holding &"MESH"
    rodata += struct.pack(">I", mesh_va)
    toc = rodata_va + 0x8000
    opd_off = rodata_off + 0x40
    opd_va = BASE + opd_off
    code = [
        lis(3, 0x5445), ori(3, 3, 0x5852),        # 'TEXR' immediate (BE)
        addi(11, 0, 801), 0x44000002,              # li r11,801 ; sc
        lis(4, rodata_va >> 16), addi(4, 4, (rodata_va & 0xFFFF) - (0x10000 if rodata_va & 0x8000 else 0)),
        lwz(5, 2, slot_va - toc),                  # lwz r5, slot(r2) -> "MESH"
        0x4E800020,                                # blr
    ]
    text = b"".join(struct.pack(">I", w) for w in code)
    opd = struct.pack(">II", text_va, toc)
    shstr = b"\0.text\0.rodata\0.opd\0.shstrtab\0"
    shstr_off = opd_off + len(opd)
    sh_off = (shstr_off + len(shstr) + 7) & ~7
    img = bytearray(sh_off + 5 * 64)
    img[text_off:text_off + len(text)] = text
    img[rodata_off:rodata_off + len(rodata)] = rodata
    img[opd_off:opd_off + len(opd)] = opd
    img[shstr_off:shstr_off + len(shstr)] = shstr
    ident = b"\x7fELF\x02\x02\x01\x66" + b"\0" * 8
    struct.pack_into(">16sHHIQQQIHHHHHH", img, 0, ident, 2, 0x15, 1, opd_va, 64, sh_off, 0, 64, 56, 1, 64, 5, 4)
    struct.pack_into(">IIQQQQQQ", img, 64, 1, 5, 0, BASE, BASE, shstr_off, shstr_off, 0x10000)
    secs = [
        (0, 0, 0, 0, 0, 0),
        (1, 1, 0x6, text_va, text_off, len(text)),
        (7, 1, 0x2, rodata_va, rodata_off, len(rodata)),
        (15, 1, 0x3, opd_va, opd_off, len(opd)),
        (20, 3, 0, 0, shstr_off, len(shstr)),
    ]
    for i, (name, typ, flags, addr, off, size) in enumerate(secs):
        struct.pack_into(">IIQQQQIIQQ", img, sh_off + i * 64, name, typ, flags, addr, off, size, 0, 0, 4, 0)
    with open(path, "wb") as f:
        f.write(img)
    return text_va


def main():
    out = "/work/test_eboot_scan"
    os.makedirs(out, exist_ok=True)
    elf_path = os.path.join(out, "fake.elf")
    text_va = build(elf_path)
    sys.argv = ["eboot_scan.py", "--elf", elf_path, "--out", out, "--tags", "TEXR,MESH"]
    eboot_scan.main()

    sc = open(os.path.join(out, "syscalls.csv")).read()
    xr = open(os.path.join(out, "tag_xrefs.md")).read()
    checks = {
        "sc com r11=801": f"0x{text_va + 12:08X},0x{text_va:08X},801" in sc,
        "TEXR imediato": "imediato BE 0x54455852" in xr,
        "TEXR string via lis/addi": "\"LoadTEXR\" via abs" in xr,
        "MESH via slot da TOC": "\"MESH\" via toc slot" in xr,
        "funcao do .opd": f"funcao 0x{text_va:08X}" in xr,
    }
    for name, ok in checks.items():
        print(("OK   " if ok else "FALHA") + " " + name)
    sys.exit(0 if all(checks.values()) else 1)


if __name__ == "__main__":
    main()
