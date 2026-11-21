# ============================================================================
#  NovaOS - boot chain blobs embedded in the kernel image
#
#  The `install` wizard needs the two boot-chain pieces to write to disk:
#  boothd.bin (HDD stage 1, written by the Makefile) and loader.bin (stage 2,
#  identical to the floppy path).  The kernel image itself is copied from
#  RAM at 0x100000, so only these two have to be embedded at build time.
# ============================================================================
    .section .rodata
    .globl boothd_bin, boothd_bin_end, loader_bin, loader_bin_end

    .balign 4
boothd_bin:
    .incbin "build/boothd.bin"
boothd_bin_end:

    .balign 4
loader_bin:
    .incbin "build/loader.bin"
loader_bin_end:
