#!/usr/bin/env python3
"""NovaOS - build a 1.44MB FAT12 data disk image.

Usage: mkdata.py <out.img> <hello.elf> [extra files...]

Layout: standard FAT12 floppy geometry (2 heads, 18 sectors/track):
  sector 0      : boot sector with a valid BPB (data-only, not bootable)
  sectors 1-9   : FAT #1
  sectors 10-18 : FAT #2
  sectors 19-32 : root directory (224 entries)
  sectors 33+   : data area
Files are packed with contiguous cluster chains, starting at cluster 2.
"""
import struct
import sys

SECTORS = 2880
SPT = 18
HEADS = 2
RESERVED = 1
NUM_FATS = 2
FAT_SIZE = 9
ROOT_ENTRIES = 224
BYTES = 512


def main() -> None:
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    out_path, elf_path = sys.argv[1], sys.argv[2]
    extra = sys.argv[3:]

    files = []          # (name_8_3, data_bytes)
    with open(elf_path, "rb") as f:
        elf_data = f.read()
    files.append(("HELLO   ELF", elf_data))
    files.append(("HELLO   TXT", b"Hello from the NovaOS FAT12 data disk!\n"
                                 b"Created by tools/mkdata.py.\n"))
    files.append(("README  TXT", b"NovaOS data disk - FAT12\n"
                                 b"Files: HELLO.ELF (ring3 demo program)\n"
                                 b"       HELLO.TXT, README.TXT\n"))
    for p in extra:
        with open(p, "rb") as f:
            data = f.read()
        base = p.split("/")[-1].upper().replace("-", "_")
        name8 = (base[:8] + "       ")[:8]
        ext = "TXT"
        if "." in base:
            name8 = (base.split(".")[0][:8] + "       ")[:8]
            ext = (base.split(".")[1][:3] + "   ")[:3]
        files.append((name8 + ext, data))

    root_sectors = (ROOT_ENTRIES * 32 + BYTES - 1) // BYTES
    data_lba = RESERVED + NUM_FATS * FAT_SIZE + root_sectors
    spc = 1  # sectors per cluster

    # --- build disk -------------------------------------------------------
    disk = bytearray(SECTORS * BYTES)

    def write_sector(lba: int, data: bytes) -> None:
        off = lba * BYTES
        disk[off:off + len(data)] = data

    # boot sector / BPB
    bpb = bytearray(BYTES)
    bpb[0:3] = b"\xeb\x3c\x90"                 # jmp + nop
    bpb[3:11] = b"NOVAOS  "                    # OEM
    struct.pack_into("<H", bpb, 11, BYTES)     # bytes per sector
    bpb[13] = spc                              # sectors per cluster
    struct.pack_into("<H", bpb, 14, RESERVED)
    bpb[16] = NUM_FATS
    struct.pack_into("<H", bpb, 17, ROOT_ENTRIES)
    struct.pack_into("<H", bpb, 19, SECTORS)   # total sectors (16-bit)
    bpb[21] = 0xF0                             # media descriptor
    struct.pack_into("<H", bpb, 22, FAT_SIZE)
    struct.pack_into("<H", bpb, 24, SPT)
    struct.pack_into("<H", bpb, 26, HEADS)
    bpb[510:512] = b"\x55\xaa"
    write_sector(0, bytes(bpb))

    # FATs (cluster 0:0xFF0, cluster 1:0xFFF, then chain, EOC at end)
    nclusters = (SECTORS - data_lba) // spc
    fat_entries = [0xFF0, 0xFFF]
    cluster = 2
    for _, data in files:
        need = (len(data) + BYTES - 1) // BYTES
        for i in range(need):
            fat_entries.append(cluster + i + 1 if i + 1 < need else 0xFFF)
        cluster += need
    while len(fat_entries) < nclusters + 2:
        fat_entries.append(0x000)

    def fat12_bytes(entries):
        """Pack FAT12 entries: each pair of 12-bit entries fills 3 bytes."""
        out = bytearray()
        i = 0
        while i + 1 < len(entries):
            a, b = entries[i], entries[i + 1]
            out.append(a & 0xFF)
            out.append(((a >> 8) & 0x0F) | ((b & 0x0F) << 4))
            out.append((b >> 4) & 0xFF)
            i += 2
        return bytes(out)

    fat = fat12_bytes(fat_entries)
    fat = fat.ljust(FAT_SIZE * BYTES, b"\x00")
    for f in range(NUM_FATS):
        write_sector(RESERVED + f * FAT_SIZE, fat[:FAT_SIZE * BYTES])

    # root directory
    root = bytearray(ROOT_ENTRIES * 32)
    off = 0
    cluster = 2
    for name, data in files:
        e = bytearray(32)
        e[0:11] = name.encode("ascii")
        e[11] = 0x20
        struct.pack_into("<H", e, 26, cluster)
        struct.pack_into("<I", e, 28, len(data))
        root[off:off + 32] = e
        off += 32
        cluster += (len(data) + BYTES - 1) // BYTES
    write_sector(RESERVED + NUM_FATS * FAT_SIZE, bytes(root[:root_sectors * BYTES]))

    # data area
    cluster = 2
    for name, data in files:
        lba = data_lba + (cluster - 2) * spc
        for i in range(0, len(data), BYTES):
            chunk = data[i:i + BYTES]
            write_sector(lba, chunk)
            lba += 1
        cluster += (len(data) + BYTES - 1) // BYTES

    with open(out_path, "wb") as f:
        f.write(disk)
    print(f"==> {out_path}: {len(disk)} bytes, {len(files)} files, "
          f"data starts at LBA {data_lba}")


if __name__ == "__main__":
    main()
