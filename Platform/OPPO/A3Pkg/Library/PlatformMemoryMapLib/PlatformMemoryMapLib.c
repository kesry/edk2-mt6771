#include <Library/BaseLib.h>
#include <Library/PlatformMemoryMapLib.h>

/*
 * OPPO A3 (PADT00, MT6771) memory map.
 *
 * Evidence:
 *  - Original Android_boot.img DTB (parsed offline): /memory reg =
 *    <0x40000000 0x1e605000>; 4 GiB DRAM minus ~1.6 MiB reserved at the top.
 *  - mainline-linux-mt6771 patch/0001: framebuffer 0x7d170000, 16 MiB,
 *    from the LK-provided chosen/atag,videolfb.
 *  - mainline-linux-mt6771 docs/boot打包.md: boot partition @ mmcblk0p35,
 *    page size 2048, 32 MiB image; LK applies a vendor DTBO overlay.
 *
 * All entries marked TODO are inherited from the MT8183/MT6771 reference
 * board packages and still need on-device validation for this exact board.
 */
static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] = {
/*                                                    EFI_RESOURCE_ EFI_RESOURCE_ATTRIBUTE_ EFI_MEMORY_TYPE ARM_REGION_ATTRIBUTE_
     MemLabel(32 Char.),  MemBase,    MemSize, BuildHob, ResourceType, ResourceAttribute, MemoryType, CacheAttributes
--------------------- Register ---------------------*/
    {"Periphs",           0x00000000, 0x1B000000,  AddMem, MEM_RES, UNCACHEABLE,  RtCode,   NS_DEVICE},

    //--------------------- DDR --------------------- */
    // 0x40000000 - 0x5e605000 (original DTB: 0x1e605000 bytes)
    {"DDR Memory 1",      0x40000000, 0x00C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
    {"UEFI Stack",        0x40C00000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
    {"CPU Vectors",       0x40C40000, 0x00010000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},
    {"DXE Heap",          0x40C50000, 0x075b0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"DDR Memory 2",      0x48400000, 0x07c00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"UEFI FD",           0x50000000, 0x00700000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},
    {"DDR Memory 3",      0x50700000, 0x03f00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    // Original DTB: ramoops at 0x54410000, size 0x000e0000 (TODO: preserve across UEFI boot?)
    {"ATF Mem",           0x54600000, 0x00050000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"DDR Memory 4",      0x54650000, 0x009b0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"RamDisk",           0x55000000, 0x01000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"DDR Memory 5",      0x56000000, 0x1a600000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    // Distinct MemoryMaps (TODO: validate on MT6771 / OPPO A3)
    {"TEE Memory",        0x70000000, 0x03c00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"TrustZone SecMem",  0x7fc00000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    // Framebuffer: 0x7d170000, 16 MiB, 1080x2280 a8r8g8b8 (mainline patch/0001)
    {"Framebuffer",       0x7d170000, 0x01000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
    {"DDR Memory 6",      0x7e170000, 0x01e90000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"SSPM Mem",          0x7ff00000, 0x000c0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"DDR Memory 7",      0x7ffc0000, 0x1cf40000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"SCP Mem",           0x9cf00000, 0x00600000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"DDR Memory 8",      0x9d500000, 0x000f0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"VPU Mem",           0x9d5f0000, 0x02a10000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"DDR Memory 9",      0xa0000000, 0xa0000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
//------------------- Terminator for MMU ---------------------
{"Terminator", 0, 0, 0, 0, 0, 0, 0}};

ARM_MEMORY_REGION_DESCRIPTOR_EX *GetPlatformMemoryMap()
{
  return gDeviceMemoryDescriptorEx;
}
