#!/usr/bin/env python3
"""Take apart a serialized MTLBinaryArchive and get at the GPU machine code.

Layout, as observed on macOS 26.6.2 / Apple M5 Pro (AGX G17X):

    fat file
      slice cputype 0x01000017  MetalLib (AIR bitcode), not used here
      slice cputype 0x01000013  "applegpu" Mach-O
        __TEXT,__compute        a nested Mach-O object (one per pipeline stage)
          __TEXT,__text         the GPU machine code
          symbols               _agc.main, _agc.main.constant_program, ...
          __GPU_*_MD            driver metadata (not decoded here)

Only container formats are parsed. Nothing in here interprets the code bytes.

usage:
    agxbin.py info <archive>
    agxbin.py code <archive>            hex dump of each code symbol
    agxbin.py diff <archive-a> <archive-b>
"""
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

FAT_MAGIC = 0xCAFEBABE  # stored big-endian; the first bytes of the file read cb fe ba be
FAT_MAGIC_ALT = 0xCBFEBABE
MH_MAGIC_64 = 0xFEEDFACF
CPU_APPLEGPU = 0x01000013
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
STAGE_SECTIONS = ("__compute", "__vertex", "__fragment")


@dataclass
class Section:
    segname: str
    sectname: str
    addr: int
    size: int
    offset: int
    data: bytes


@dataclass
class Symbol:
    name: str
    sect: int
    value: int


@dataclass
class MachO:
    cputype: int
    cpusubtype: int
    filetype: int
    sections: list = field(default_factory=list)
    symbols: list = field(default_factory=list)

    def section(self, sectname, segname=None):
        for s in self.sections:
            if s.sectname == sectname and (segname is None or s.segname == segname):
                return s
        return None


def _cstr(raw):
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def parse_macho(data):
    magic, cputype, cpusubtype, filetype, ncmds, _sizeofcmds, _flags, _res = struct.unpack_from(
        "<IIIIIIII", data, 0
    )
    if magic != MH_MAGIC_64:
        raise ValueError(f"not a 64-bit Mach-O (magic {magic:#x})")

    m = MachO(cputype, cpusubtype, filetype)
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == LC_SEGMENT_64:
            segname = _cstr(data[off + 8 : off + 24])
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            for i in range(nsects):
                so = off + 72 + i * 80
                sectname = _cstr(data[so : so + 16])
                ssegname = _cstr(data[so + 16 : so + 32]) or segname
                addr, size, foff = struct.unpack_from("<QQI", data, so + 32)
                m.sections.append(Section(ssegname, sectname, addr, size, foff, data[foff : foff + size]))
        elif cmd == LC_SYMTAB:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", data, off + 8)
            strtab = data[stroff : stroff + strsize]
            for i in range(nsyms):
                strx, _ntype, nsect, _ndesc, value = struct.unpack_from("<IBBHQ", data, symoff + i * 16)
                m.symbols.append(Symbol(_cstr(strtab[strx:]), nsect, value))
        off += cmdsize
    return m


def gpu_slice(data):
    """Return the applegpu Mach-O slice of a serialized binary archive."""
    magic, nfat = struct.unpack_from(">II", data, 0)
    if magic not in (FAT_MAGIC, FAT_MAGIC_ALT):
        raise ValueError(f"not a fat binary archive (magic {magic:#x})")
    for i in range(nfat):
        cputype, _sub, off, size, _align = struct.unpack_from(">IIIII", data, 8 + i * 20)
        if cputype == CPU_APPLEGPU:
            return data[off : off + size]
    raise ValueError("no applegpu slice in archive")


@dataclass
class Stage:
    name: str      # __compute, __vertex or __fragment
    obj: MachO     # the nested object
    text: bytes    # its __TEXT,__text
    funcs: dict    # symbol name -> code bytes, in address order


def stages(path):
    outer = parse_macho(gpu_slice(Path(path).read_bytes()))
    found = []
    for sect in outer.sections:
        if sect.sectname not in STAGE_SECTIONS:
            continue
        obj = parse_macho(sect.data)
        text_index = next(
            (i + 1 for i, s in enumerate(obj.sections) if s.sectname == "__text"), None
        )
        if text_index is None:
            continue
        text = obj.sections[text_index - 1].data
        syms = sorted((s for s in obj.symbols if s.sect == text_index), key=lambda s: s.value)
        funcs = {}
        for i, s in enumerate(syms):
            end = syms[i + 1].value if i + 1 < len(syms) else len(text)
            funcs[s.name] = text[s.value : end]
        found.append(Stage(sect.sectname, obj, text, funcs))
    return found


def main_code(path):
    """Code bytes of the kernel's entry point (_agc.main) in the first stage."""
    for st in stages(path):
        for name, code in st.funcs.items():
            if name == "_agc.main":
                return code
    raise ValueError(f"{path}: no _agc.main")


def hexs(b):
    return " ".join(f"{x:02x}" for x in b)


def cmd_info(path):
    outer = parse_macho(gpu_slice(Path(path).read_bytes()))
    print(f"applegpu slice: cpusubtype {outer.cpusubtype:#x}, filetype {outer.filetype}")
    for s in outer.sections:
        print(f"  {s.segname},{s.sectname:<14} {s.size:6d} bytes")
    for st in stages(path):
        print(f"\nstage {st.name}: nested object, cpusubtype {st.obj.cpusubtype:#x}")
        for s in st.obj.sections:
            print(f"  {s.segname:<18} {s.sectname:<10} {s.size:6d} bytes")
        for name, code in st.funcs.items():
            print(f"  symbol {name:<32} {len(code):4d} bytes of code")


def cmd_code(path):
    for st in stages(path):
        for name, code in st.funcs.items():
            print(f"{st.name} {name} ({len(code)} bytes)")
            for i in range(0, len(code), 16):
                print(f"  {i:04x}: {hexs(code[i:i + 16])}")


def cmd_diff(a, b):
    ca, cb = main_code(a), main_code(b)
    print(f"a: {len(ca)} bytes   b: {len(cb)} bytes")
    if len(ca) != len(cb):
        print("lengths differ; showing both")
        print("  a:", hexs(ca))
        print("  b:", hexs(cb))
        return
    for i, (x, y) in enumerate(zip(ca, cb)):
        if x != y:
            print(f"  byte {i:3d}: {x:02x} -> {y:02x}   xor {x ^ y:08b}")


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ("info", "code", "diff"):
        sys.exit(__doc__)
    if sys.argv[1] == "info":
        cmd_info(sys.argv[2])
    elif sys.argv[1] == "code":
        cmd_code(sys.argv[2])
    else:
        cmd_diff(sys.argv[2], sys.argv[3])
