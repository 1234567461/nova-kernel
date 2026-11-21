#!/usr/bin/env python3
"""
pe-reloc.py - give a gnu-efi PE32+ image a complete base relocation table.

Why this exists
---------------
binutils' objcopy --target=efi-app-x86_64 does NOT translate the ELF
`.rela.dyn` R_X86_64_RELATIVE entries into a PE `.reloc` directory.  The
resulting BOOTX64.EFI ships with an (almost) empty relocation table, so when
firmware (OVMF/Apple/VMware) loads the image somewhere other than its
preferred ImageBase it cannot fix up the GOT / global pointers.  The loader
then calls / jumps to a raw, non-rebased offset (observed as RIP=0 or
CR2=0xFFFFFFFFFFFFB8CF) and the machine dies with #PF before the kernel runs.

This script reads the relocations from the ELF sibling image and rewrites the
PE's .reloc section with proper IMAGE_REL_BASED_DIR64 blocks, padding the raw
data to FileAlignment and pointing the existing .reloc section header at it.

Usage:  pe-reloc.py <elf-input> <pe-file-to-patch>
"""
import struct
import sys

SHT_RELA = 4
R_X86_64_RELATIVE = 8
IMAGE_REL_BASED_DIR64 = 10


def read_elf_rela(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 2:
        raise SystemExit("not an ELF64 file: " + path)
    e_shoff, = struct.unpack_from("<Q", d, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", d, 0x3A)
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, offset, size, link, info, align, entsize = \
            struct.unpack_from("<IIQQQQIIQQ", d, off)
        secs.append(dict(name=name, type=typ, addr=addr, offset=offset,
                         size=size, entsize=entsize))
    relocs = []
    for s in secs:
        # every RELA section (".rela.dyn" and friends) may hold RELATIVE entries
        if s["type"] != SHT_RELA or s["size"] == 0:
            continue
        n = s["size"] // 24
        for i in range(n):
            r_off, r_info, r_add = struct.unpack_from("<QQq", d, s["offset"] + i * 24)
            if (r_info & 0xFFFFFFFF) == R_X86_64_RELATIVE:
                relocs.append(r_off)
    return sorted(set(relocs))


def build_blocks(relocs, image_base):
    """Group RVAs into IMAGE_REL_BASED_DIR64 blocks, 4-byte aligned."""
    blocks = []
    page, entries = None, []

    def flush():
        if page is None:
            return
        size = 8 + len(entries) * 2
        size = (size + 3) & ~3
        body = struct.pack("<II", page, size) + b"".join(entries)
        body += b"\x00" * (size - (8 + len(entries) * 2))
        blocks.append(body)

    for rva in relocs:
        rva -= image_base
        p = rva & ~0xFFF
        if p != page:
            flush()
            page, entries = p, []
        entries.append(struct.pack("<H", (IMAGE_REL_BASED_DIR64 << 12) | (rva & 0xFFF)))
    flush()
    return b"".join(blocks)


def patch_pe(path, blob, nreloc):
    d = bytearray(open(path, "rb").read())
    if d[:2] != b"MZ":
        raise SystemExit("not a PE file: " + path)
    pe, = struct.unpack_from("<I", d, 0x3C)
    if d[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("missing PE signature")
    coff = pe + 4
    nsec, = struct.unpack_from("<H", d, coff + 2)
    opt_size, = struct.unpack_from("<H", d, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from("<H", d, opt)
    if magic != 0x20B:
        raise SystemExit("expected PE32+ (0x20B), got 0x%X" % magic)
    image_base, = struct.unpack_from("<Q", d, opt + 24)
    file_align, = struct.unpack_from("<I", d, opt + 36)
    size_of_image, = struct.unpack_from("<I", d, opt + 56)
    sec_tab = opt + opt_size

    reloc_hdr = None
    for i in range(nsec):
        h = sec_tab + i * 40
        name = bytes(d[h:h + 8]).rstrip(b"\0")
        if name == b".reloc":
            reloc_hdr = h
            break
    if reloc_hdr is None:
        raise SystemExit("no .reloc section to patch")

    va, = struct.unpack_from("<I", d, reloc_hdr + 12)
    vsize, = struct.unpack_from("<I", d, reloc_hdr + 8)

    raw = (len(d) + file_align - 1) // file_align * file_align
    d.extend(b"\x00" * (raw - len(d)))
    d.extend(blob)
    padded = (len(blob) + file_align - 1) // file_align * file_align
    d.extend(b"\x00" * (padded - len(blob)))

    struct.pack_into("<I", d, reloc_hdr + 16, padded)        # SizeOfRawData
    struct.pack_into("<I", d, reloc_hdr + 20, raw)           # PointerToRawData
    struct.pack_into("<I", d, reloc_hdr + 8, len(blob))      # VirtualSize

    need = va + ((len(blob) + 0xFFF) & ~0xFFF)
    if need > size_of_image:
        struct.pack_into("<I", d, opt + 56, need)

    open(path, "wb").write(d)
    return image_base, va, len(blob), nreloc


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: pe-reloc.py <elf> <pe>")
    elf, pe = sys.argv[1], sys.argv[2]
    relocs = read_elf_rela(elf)
    if not relocs:
        raise SystemExit("no R_X86_64_RELATIVE relocations found in " + elf)
    # ImageBase is only known from the PE; read it first with a zero guess
    d = open(pe, "rb").read()
    peoff, = struct.unpack_from("<I", d, 0x3C)
    opt = peoff + 4 + 20
    image_base, = struct.unpack_from("<Q", d, opt + 24)
    blob = build_blocks(relocs, image_base)
    ib, va, n, cnt = patch_pe(pe, blob, len(relocs))
    print("pe-reloc: %d relocations -> %d bytes at RVA 0x%X (ImageBase 0x%X)"
          % (cnt, n, va, ib))


if __name__ == "__main__":
    main()
