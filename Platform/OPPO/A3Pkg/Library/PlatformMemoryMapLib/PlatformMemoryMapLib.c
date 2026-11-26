#include <Library/BaseLib.h>
#include <Library/PlatformMemoryMapLib.h>

/*
 * OPPO A3 (PADT00, MT6771) memory map.
 *
 * Evidence:
 *  - DRAM base 0x40000000, 4 GiB total (original Android_boot.img DTB memory
 *    node reports the first bank as 0x1e605000 bytes; mainline README records
 *    System RAM ending at 0x13fffefff).
 *  - Framebuffer: LK injects simple-framebuffer via its odmdtbo overlay.
 *    The bootable device tree (mainline vendor/a3-v186-tgpio.dtb) decoded at
 *    0x16ef0 gives:
 *        framebuffer@77440000
 *        reg    = <0x77440000 0x01000000>   (16 MiB)
 *        width  = 1080
 *        height = 2280
 *        stride = 0x1100 (4352 bytes = 1088 px)
 *        format = "a8r8g8b8"
 *    An earlier mainline patch used 0x7d170000, which is NOT what LK sets up.
 *  - Reserved regions (ATF, TEE, SSPM, SCP, VPU) follow the reference MT8183
 *    platform package; TODO: validate each on this board.
 *
 * The table must stay ascending and non-overlapping: the PrePi MMU code walks
 * it directly, and an unmapped hole in the framebuffer range causes a data
 * abort as soon as SimpleFbDxe initialises the console.
 */
static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] = {
/*                                                    EFI_RESOURCE_ EFI_RESOURCE_ATTRIBUTE_ EFI_MEMORY_TYPE ARM_REGION_ATTRIBUTE_
     MemLabel(32 Char.),  MemBase,    MemSize, BuildHob, ResourceType, ResourceAttribute, MemoryType, CacheAttributes
--------------------- Register ---------------------*/
    {"Periphs",           0x00000000, 0x1B000000,  AddMem, MEM_RES, UNCACHEABLE,  RtCode,   NS_DEVICE},

    //--------------------- DDR 0x40000000 - 0x140000000 (4 GiB) ------------ */
    {"DDR Memory 1",      0x40000000, 0x00C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
    {"UEFI Stack",        0x40C00000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
    {"CPU Vectors",       0x40C40000, 0x00010000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},
    {"DXE Heap",          0x40C50000, 0x075B0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"DDR Memory 2",      0x48200000, 0x07E00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"UEFI FD",           0x50000000, 0x00700000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},
    {"DDR Memory 3",      0x50700000, 0x03F00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"ATF Mem",           0x54600000, 0x00050000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"DDR Memory 4",      0x54650000, 0x009B0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"RamDisk",           0x55000000, 0x01000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"DDR Memory 5",      0x56000000, 0x1A000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"TEE Memory",        0x70000000, 0x03C00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"DDR Memory 6",      0x73C00000, 0x03840000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    // LK simple-framebuffer: base 0x77440000, 16 MiB, 1080x2280, stride 4352
    {"Framebuffer",       0x77440000, 0x01000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
    {"DDR Memory 7",      0x78440000, 0x077C0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"TrustZone SecMem",  0x7FC00000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"DDR Memory 8",      0x7FE00000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"SSPM Mem",          0x7FF00000, 0x000C0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"DDR Memory 9",      0x7FFC0000, 0x1CF40000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"SCP Mem",           0x9CF00000, 0x00600000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"DDR Memory 10",     0x9D500000, 0x000F0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"VPU Mem",           0x9D5F0000, 0x02A10000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"DDR Memory 11",     0xA0000000, 0xA0000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
//------------------- Terminator for MMU ---------------------
{"Terminator", 0, 0, 0, 0, 0, 0, 0}};

ARM_MEMORY_REGION_DESCRIPTOR_EX *GetPlatformMemoryMap()
{
  return gDeviceMemoryDescriptorEx;
}
