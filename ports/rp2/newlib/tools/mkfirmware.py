# SPDX-FileCopyrightText: 2024 Gregory Neverov
# SPDX-License-Identifier: MIT

import argparse
import elf as elf32
from elfmodel import *
import os


parser = argparse.ArgumentParser(description="Processes a firmware ELF file for dynamic linking")
parser.add_argument("input", help="input ELF file")
parser.add_argument("--output", help="output ELF file")
args = parser.parse_args()

module_name = args.input
elffile = open_elffile(module_name)


dynstr = StrtabSection(".dynstr", sh_flags=elf32.SHF_ALLOC)
dynsym = SymtabSection(".dynsym", link=dynstr, sh_type=elf32.SHT_DYNSYM, sh_flags=elf32.SHF_ALLOC)
dynhash = Section(
    ".hash",
    link=dynsym,
    sh_type=elf32.SHT_HASH,
    sh_flags=elf32.SHF_ALLOC,
    sh_addralign=sizeof(elf32.Word),
)
dynamic = DynamicSection(".dynamic", link=dynstr, sh_flags=elf32.SHF_ALLOC)


def mk_dyn(sym):
    if not hasattr(sym, "dyn"):
        dsym = Symbol(
            sym.name,
            sym.section,
            st_value=sym.struct.st_value,
            st_size=sym.struct.st_size,
            st_info=sym.struct.st_info,
            st_other=sym.struct.st_other,
            st_shndx=sym.struct.st_shndx,
        )
        dynsym.symbols.append(dsym)
        sym.dyn = dsym
    return sym.dyn


for section in elffile.iter_sections(elf32.SHT_SYMTAB):
    for sym in section.symbols:
        if not sym.section:
            continue
        if sym.struct.st_bind != elf32.STB_LOCAL and sym.struct.st_visibility == elf32.STV_DEFAULT:
            mk_dyn(sym)


dynamic.dyns.append(DynEntry(d_tag=elf32.DT_HASH, value=lambda: dynhash.struct.sh_addr))
dynhash.data = int.to_bytes(0, sizeof(elf32.Word), "little") + int.to_bytes(
    len(dynsym.symbols), sizeof(elf32.Word), "little"
)

dynamic.dyns.append(DynEntry(d_tag=elf32.DT_STRTAB, value=lambda: dynstr.struct.sh_addr))

dynamic.dyns.append(DynEntry(d_tag=elf32.DT_SYMTAB, value=lambda: dynsym.struct.sh_addr))

dynamic.dyns.append(DynEntry(d_tag=elf32.DT_STRSZ, value=lambda: dynstr.size))

dynamic.dyns.append(DynEntry(d_tag=elf32.DT_SYMENT, d_val=dynsym.struct.sh_entsize))

dynamic.dyns.append(DynEntry(d_tag=elf32.DT_SONAME, value=os.path.basename(module_name)))

dynamic.dyns.append(DynEntry())


flash_heap = [sh for sh in elffile.sections if sh.name == ".flash_heap"][0]
flash_heap_end = Section(
    ".flash_heap_end",
    sh_type=elf32.SHT_PROGBITS,
    sh_flags=elf32.SHF_ALLOC,
    sh_addralign=4,
    data=flash_heap.data,
)
i = elffile.sections.index(flash_heap) + 1
elffile.sections[i:i] = [dynamic, dynhash, dynstr, dynsym, flash_heap_end]

dynamic_segment = Segment(p_type=elf32.PT_DYNAMIC, sections=[dynamic])
dyn_load_segment = Segment(
    p_type=elf32.PT_LOAD, sections=[dynamic, dynhash, dynstr, dynsym, flash_heap_end]
)
elffile.segments.extend([dynamic_segment, dyn_load_segment])


IndexNodes().visit(elffile)
RegisterStrings().visit(elffile)
BuildStrtabs().visit(elffile)
ComputeAddresses().visit(elffile)
ComputeOffsets().visit(elffile)
ComputeSegments().visit(elffile)


# class FlashHeapHeader(ctypes.Structure):
#     _fields_ = [
#         ("type", ctypes.c_uint32),
#         ("flash_size", ctypes.c_uint32),
#         ("ram_size", ctypes.c_uint32),
#         ("ram_base", ctypes.c_void_p),
#         ("entry", ctypes.c_void_p),
#     ]
buf = bytearray(flash_heap.data)
buf[0:4] = int(1).to_bytes(4, "little")
buf[4:8] = (flash_heap_end.struct.sh_addr - flash_heap.struct.sh_addr).to_bytes(4, "little")
buf[16:20] = dynamic.struct.sh_addr.to_bytes(4, "little")
flash_heap.data = buf


out_module_name = args.output or module_name
fp = open(out_module_name, "wb")
WriteData(fp=fp).visit(elffile)
WriteHeaders(fp=fp).visit(elffile)
fp.close()
