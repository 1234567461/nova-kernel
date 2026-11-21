/* ============================================================================
 *  NovaOS - native UEFI (x64) loader
 *
 *  The stock boot chain (boot.asm stage1 + loader.asm stage2) is BIOS-only:
 *  it needs INT 13h/INT 10h, which no UEFI firmware provides.  This module
 *  is an alternative stage-1/2 for machines booted in pure UEFI mode:
 *
 *    1. read EFI\BOOT\KERNEL.BIN off the boot FAT volume
 *    2. AllocateAddress it at 0x100000 (the address link.ld fixes)
 *    3. ExitBootServices()
 *    4. build the exact page tables stage-2 would have built:
 *         page directory 0x9000, page table 0xA000, identity map 0..4MB
 *    5. drop from 64-bit long mode back to flat 32-bit protected mode
 *       (clear CR0.PG -> EFER.LME -> CR4.PAE, load CR3, re-enable PG)
 *    6. jump to the kernel entry at 0x100000; from here on the kernel is
 *       oblivious to how it was booted (entry.c sets its own stack, GDT,
 *       IDT and paging just like on the BIOS path)
 *
 *  Build (see Makefile "efi" target) with gnu-efi, linked as an EFI PE32+
 *  application (BOOTX64.EFI) that lives on an El Torito -e boot image.
 * ========================================================================== */
#include <efi.h>
#include <efilib.h>

#define KERNEL_LOAD     0x100000u
#define KERNEL_MAX      (200u * 512u)     /* matches boot.asm stage1 limit */
#define PD_ADDR         0x9000u           /* KERNEL_PD - must match paging.c */
#define PT_ADDR         0xA000u           /* KERNEL_PT - must match paging.c */

extern void efi_go32(UINTN kernel_entry, UINTN page_dir);  /* boot/efi/switch.S */

/* Raw COM1 writer: Boot Services (and therefore Print) are gone by the time
 * we hand over, so keep a tiny serial trace for the mode switch. */
static void com1(const char *s) {
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        __asm__ __volatile__("outb %0, %1" : : "a"(c), "Nd"((unsigned short)0x3F8));
    }
}
static void com1_hex(UINTN v) {
    char b[19]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) { int d = (v >> (60 - i * 4)) & 0xF; b[2 + i] = d < 10 ? '0' + d : 'a' + d - 10; }
    b[18] = 0; com1(b);
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    com1("[efi] step:libinit\n");
    InitializeLib(ImageHandle, SystemTable);

    com1("[efi] step:banner\n");
    Print(L"NovaOS UEFI loader v0.9\n");

    /* ---- locate the boot volume ---- */
    EFI_STATUS st;
    EFI_LOADED_IMAGE *li = NULL;
    EFI_FILE_IO_INTERFACE *fs = NULL;
    EFI_FILE *root = NULL, *f = NULL;

    st = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle,
                           com1("[efi] step:loadedimage\n");
    &LoadedImageProtocol, (void **)&li);
    if (EFI_ERROR(st)) { Print(L"FAIL: LoadedImage (%r)\n", st); return st; }

    st = uefi_call_wrapper(BS->HandleProtocol, 3, li->DeviceHandle,
                           com1("[efi] step:fsproto\n");
    &FileSystemProtocol, (void **)&fs);
    if (EFI_ERROR(st)) {
        /* fallback: first filesystem handle in the system */
        UINTN n = 0; EFI_HANDLE *h = NULL;
        st = uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol,
                               &FileSystemProtocol, NULL, &n, &h);
        if (EFI_ERROR(st) || n == 0) { Print(L"FAIL: no FAT volume (%r)\n", st); return st; }
        st = uefi_call_wrapper(BS->HandleProtocol, 3, h[0],
                               &FileSystemProtocol, (void **)&fs);
        if (EFI_ERROR(st)) return st;
    }

    st = com1("[efi] step:openvolume\n");
    uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
    if (EFI_ERROR(st)) { Print(L"FAIL: OpenVolume (%r)\n", st); return st; }

    /* kernel.bin lives next to BOOTX64.EFI on the boot image */
    st = uefi_call_wrapper(root->Open, 5, root, &f,
                           L"EFI\\BOOT\\KERNEL.BIN",
                           com1("[efi] step:openkernel\n");
    EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) { Print(L"FAIL: kernel.bin not found (%r)\n", st); return st; }

    UINT64 fsize = 0; UINTN sz = sizeof(fsize);
    com1("[efi] step:fileinfo\n");
    EFI_FILE_INFO *fi = LibFileInfo(f);
    if (fi) { fsize = fi->FileSize; FreePool(fi); }
    if (fsize == 0 || fsize > KERNEL_MAX) {
        Print(L"FAIL: bad kernel.bin size %d\n", (UINT32)fsize);
        return EFI_LOAD_ERROR;
    }
    Print(L"kernel.bin: %d bytes\n", (UINT32)fsize);

    /* ---- claim the fixed load address ---- */
    UINTN pages = (fsize + 0xFFF) >> 12;
    st = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress,
                           com1("[efi] step:allocpages\n");
    EfiLoaderData, pages, (EFI_PHYSICAL_ADDRESS)KERNEL_LOAD);
    if (EFI_ERROR(st)) { Print(L"FAIL: 0x100000 not allocatable (%r)\n", st); return st; }

    sz = (UINTN)fsize;
    com1("[efi] step:readkernel\n");
    st = uefi_call_wrapper(f->Read, 3, f, &sz, (void *)KERNEL_LOAD);
    if (EFI_ERROR(st)) { Print(L"FAIL: read kernel (%r)\n", st); return st; }
    uefi_call_wrapper(f->Close, 1, f);
    uefi_call_wrapper(root->Close, 1, root);

    Print(L"loaded at 0x100000 - leaving boot services\n");

    /* ---- exit boot services (retry: the map grows as we allocate) ---- */
    UINTN mmap_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    UINT8 *mmap = NULL;
    for (;;) {
        uefi_call_wrapper(BS->GetMemoryMap, 5, &mmap_size, mmap, &map_key,
                          &desc_size, &desc_ver);
        mmap_size += 2 * desc_size;
        mmap = AllocatePool(mmap_size);
        st = uefi_call_wrapper(BS->GetMemoryMap, 5, &mmap_size, mmap, &map_key,
                               &desc_size, &desc_ver);
        if (!EFI_ERROR(st)) break;
        FreePool(mmap);
    }
    st = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
    if (EFI_ERROR(st)) {   /* one retry is allowed by spec */
        uefi_call_wrapper(BS->GetMemoryMap, 5, &mmap_size, mmap, &map_key,
                          &desc_size, &desc_ver);
        st = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
        if (EFI_ERROR(st)) return st;
    }

    /* ---- boot services are gone: build the stage-2 page tables ---- */
    com1("[efi] boot services exited, building page tables\n");
    /* identity map 0..4MB: 1024 4KB pages in PT@0xA000, PDE[0] -> PT@0xA000 */
    UINT32 *pt = (UINT32 *)PT_ADDR;
    UINT32 *pd = (UINT32 *)PD_ADDR;
    UINT32 i;
    for (i = 0; i < 1024; i++) pt[i] = (i << 12) | 0x3;   /* present + rw */
    for (i = 1; i < 1024; i++) pd[i] = 0;
    pd[0] = PT_ADDR | 0x3;

    /* ---- long mode -> 32-bit protected mode -> kernel ---- */
    /*  NOTE: this MUST be a direct, PC-relative call.  Compiling a plain
     *  `efi_go32(...)` here made GCC emit a GOT-indirect call
     *  (movabs $off,%rax / add %r15,%rax / call *%rax).  Firmware only
     *  applies the PE .reloc table when it loads the image, and this link
     *  (objcopy'd -shared, 12-byte .reloc) leaves the GOT slot at 0, so the
     *  CPU jumped to RIP=0 and OVMF raised #PF 0x0E.  Inline asm with an
     *  explicit `call efi_go32` turns into R_X86_64_PLT32, which the linker
     *  resolves directly (the symbol is .hidden in switch.S). */
    com1("[efi] jumping to kernel ");
    com1_hex((UINTN)KERNEL_LOAD);
    com1(" cr3=");
    com1_hex((UINTN)PD_ADDR);
    com1("\n");
    __asm__ __volatile__("call efi_go32"
                         :
                         : "D"((UINTN)KERNEL_LOAD), "S"((UINTN)PD_ADDR)
                         : "memory");

    return EFI_SUCCESS;   /* never reached */
}
